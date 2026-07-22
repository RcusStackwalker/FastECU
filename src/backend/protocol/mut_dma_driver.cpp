#include "src/backend/protocol/mut_dma_driver.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/algorithms/protocol/mut_dma/qt_mut_dma.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/protocol/transport_legacy_compat.h"
namespace mutdma
{
namespace legacy_transport = fastecu::transport_legacy_compat;

static bool ackOk(bytes::ByteView f, bytes::Byte cA, bytes::Byte cB)
{
    return verifyFrame(f) && (f[0] == cA || f[0] == cB);
}
bool MutDmaDriver::startFreeFormLog(const QVector<Channel>& channels,
                                    bytes::Byte setupCmd, bytes::Byte listCmd)
{
    channels_ = channels;
    streaming_ = false;
    if (!init_.wake(t_))
    {
        return false;
    }
    legacy_transport::write(t_, buildSetupFrame(setupCmd, static_cast<bytes::Byte>(channels.size())));
    const bytes::Bytes resp1 = legacy_transport::read(t_, 50, FRAME_LEN);
    if (!ackOk(resp1, 0xA5, 0xB5))
    {
        return false; // ACK-1
    }
    legacy_transport::write(t_, buildIdListFrame(listCmd, channels));
    const bytes::Bytes resp2 = legacy_transport::read(t_, 50, FRAME_LEN);
    if (!ackOk(resp2, 0x05, 0x15))
    {
        return false; // ACK-2
    }
    streaming_ = true;
    return true;
}
bool MutDmaDriver::writeMemory(std::uint16_t addr, bytes::ByteView bytes)
{
    if (!init_.wake(t_))
    {
        return false;
    }
    const std::vector<MutDmaFrame> frames = buildWriteFrames(addr, bytes);
    if (frames.empty() && !bytes.empty())
    {
        return false;
    }
    for (const MutDmaFrame& f : frames)
    {
        legacy_transport::write(t_, f);
        if (!verifyFrame(legacy_transport::read(t_, 50, FRAME_LEN)))
        {
            return false; // echo ack
        }
    }
    return true;
}
bool MutDmaDriver::writeMemory(std::uint16_t addr, const QByteArray& bytes)
{
    return writeMemory(addr, bytes::view(bytes));
}
QVector<std::uint32_t> MutDmaDriver::pollOnce(int timeoutMs)
{
    const int want = 1 + responseDataLength(channels_) + 2; // id + data + csum + trailer
    const bytes::Bytes fr = legacy_transport::read(t_, timeoutMs, want);
    StreamFrame s = parseStreamFrame(fr);
    if (!s.ok)
    {
        return {};
    }
    return decodeStreamValues(channels_, s.data);
}
} // namespace mutdma
