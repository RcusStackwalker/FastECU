#include "protocol/mut_dma_driver.h"
#include "protocol/mut_dma_codec.h"
#include "protocol/mut_dma_memory.h"
#include "src/algorithms/protocol/qt_bytes.h"
namespace mutdma
{
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
    t_.write(buildSetupFrame(setupCmd, static_cast<bytes::Byte>(channels.size())));
    const bytes::Bytes resp1 = t_.read(50, FRAME_LEN);
    if (!ackOk(resp1, 0xA5, 0xB5))
    {
        return false; // ACK-1
    }
    t_.write(buildIdListFrame(listCmd, channels));
    const bytes::Bytes resp2 = t_.read(50, FRAME_LEN);
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
        t_.write(f);
        if (!verifyFrame(t_.read(50, FRAME_LEN)))
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
    const bytes::Bytes fr = t_.read(timeoutMs, want);
    StreamFrame s = parseStreamFrame(fr);
    if (!s.ok)
    {
        return {};
    }
    return decodeStreamValues(channels_, s.data);
}
} // namespace mutdma
