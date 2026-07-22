#include "src/backend/protocol/mut_dma_driver.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"

namespace mutdma
{
static bool ackOk(bytes::ByteView f, bytes::Byte cA, bytes::Byte cB)
{
    return verifyFrame(f) && (f[0] == cA || f[0] == cB);
}

namespace
{
fastecu::Status writeFrame(IKlineTransport& transport, bytes::ByteView frame)
{
    auto result = transport.write(frame);
    if (!result)
    {
        return std::unexpected(result.error());
    }
    if (*result != frame.size())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "partial K-Line write");
    }
    return {};
}
} // namespace

fastecu::Status MutDmaDriver::startFreeFormLog(
    const std::vector<Channel>& channels, bytes::Byte setupCmd, bytes::Byte listCmd,
    const fastecu::ICancellationToken& cancellation)
{
    channels_ = channels;
    streaming_ = false;
    if (auto wake = init_.wake(t_); !wake)
    {
        return wake;
    }
    const auto setup = buildSetupFrame(setupCmd, static_cast<bytes::Byte>(channels.size()));
    if (auto written = writeFrame(t_, setup); !written)
    {
        return written;
    }
    auto resp1 = t_.read(50, cancellation);
    if (!resp1)
    {
        return std::unexpected(resp1.error());
    }
    if (!resp1->has_value() || !ackOk(resp1->value(), 0xA5, 0xB5))
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse,
                             "MUT/DMA setup acknowledgement invalid");
    }
    const auto idList = buildIdListFrame(listCmd, channels);
    if (auto written = writeFrame(t_, idList); !written)
    {
        return written;
    }
    auto resp2 = t_.read(50, cancellation);
    if (!resp2)
    {
        return std::unexpected(resp2.error());
    }
    if (!resp2->has_value() || !ackOk(resp2->value(), 0x05, 0x15))
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse,
                             "MUT/DMA channel-list acknowledgement invalid");
    }
    streaming_ = true;
    return {};
}

fastecu::Status MutDmaDriver::writeMemory(
    std::uint16_t addr, bytes::ByteView data,
    const fastecu::ICancellationToken& cancellation)
{
    if (auto wake = init_.wake(t_); !wake)
    {
        return wake;
    }
    const std::vector<MutDmaFrame> frames = buildWriteFrames(addr, data);
    if (frames.empty() && !data.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "MUT/DMA memory write range is invalid");
    }
    for (const MutDmaFrame& f : frames)
    {
        if (auto written = writeFrame(t_, f); !written)
        {
            return written;
        }
        auto echo = t_.read(50, cancellation);
        if (!echo)
        {
            return std::unexpected(echo.error());
        }
        if (!echo->has_value() || !verifyFrame(echo->value()))
        {
            return fastecu::fail(fastecu::ErrorKind::BadResponse,
                                 "MUT/DMA memory-write echo invalid");
        }
    }
    return {};
}

fastecu::Result<std::vector<std::uint32_t>> MutDmaDriver::pollOnce(
    int timeoutMs, const fastecu::ICancellationToken& cancellation)
{
    if (!streaming_)
    {
        return std::vector<std::uint32_t>{};
    }
    auto frame = t_.read(timeoutMs, cancellation);
    if (!frame)
    {
        return std::unexpected(frame.error());
    }
    if (!frame->has_value())
    {
        return std::vector<std::uint32_t>{};
    }
    StreamFrame s = parseStreamFrame(frame->value());
    if (!s.ok)
    {
        return std::vector<std::uint32_t>{};
    }
    return decodeStreamValues(channels_, s.data);
}
} // namespace mutdma
