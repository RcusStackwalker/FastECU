#include "src/backend/protocol/mut_dma_driver.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"

namespace mutdma
{
static bool AckOk(bytes::ByteView f, bytes::Byte c_a, bytes::Byte c_b)
{
    return VerifyFrame(f) && (f[0] == c_a || f[0] == c_b);
}

namespace
{
using namespace std::chrono_literals;

fastecu::Status WriteFrame(IKlineTransport& transport, bytes::ByteView frame)
{
    auto result = transport.Write(frame);
    if (!result)
    {
        return std::unexpected(result.error());
    }
    if (*result != frame.size())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "partial K-Line write");
    }
    return {};
}
} // namespace

fastecu::Status MutDmaDriver::StartFreeFormLog(const std::vector<Channel>& channels, bytes::Byte setup_cmd,
                                               bytes::Byte list_cmd, const fastecu::ICancellationToken& cancellation)
{
    channels_ = channels;
    streaming_ = false;
    if (auto wake = init_.Wake(t_); !wake)
    {
        return wake;
    }
    const auto setup = BuildSetupFrame(setup_cmd, static_cast<bytes::Byte>(channels.size()));
    if (auto written = WriteFrame(t_, setup); !written)
    {
        return written;
    }
    auto resp1 = t_.Read(50ms, cancellation);
    if (!resp1)
    {
        return std::unexpected(resp1.error());
    }
    if (!resp1->has_value() || !AckOk(resp1->value(), 0xA5, 0xB5))
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "MUT/DMA setup acknowledgement invalid");
    }
    const auto id_list = BuildIdListFrame(list_cmd, channels);
    if (auto written = WriteFrame(t_, id_list); !written)
    {
        return written;
    }
    auto resp2 = t_.Read(50ms, cancellation);
    if (!resp2)
    {
        return std::unexpected(resp2.error());
    }
    if (!resp2->has_value() || !AckOk(resp2->value(), 0x05, 0x15))
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "MUT/DMA channel-list acknowledgement invalid");
    }
    streaming_ = true;
    return {};
}

fastecu::Status MutDmaDriver::WriteMemory(std::uint16_t addr, bytes::ByteView data,
                                          const fastecu::ICancellationToken& cancellation)
{
    if (auto wake = init_.Wake(t_); !wake)
    {
        return wake;
    }
    const std::vector<MutDmaFrame> frames = BuildWriteFrames(addr, data);
    if (frames.empty() && !data.empty())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "MUT/DMA memory write range is invalid");
    }
    for (const MutDmaFrame& f : frames)
    {
        if (auto written = WriteFrame(t_, f); !written)
        {
            return written;
        }
        auto echo = t_.Read(50ms, cancellation);
        if (!echo)
        {
            return std::unexpected(echo.error());
        }
        if (!echo->has_value() || !VerifyFrame(echo->value()))
        {
            return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "MUT/DMA memory-write echo invalid");
        }
    }
    return {};
}

fastecu::Result<std::vector<std::uint32_t>> MutDmaDriver::PollOnce(std::chrono::milliseconds timeout,
                                                                   const fastecu::ICancellationToken& cancellation)
{
    if (!streaming_)
    {
        return std::vector<std::uint32_t>{};
    }
    auto frame = t_.Read(timeout, cancellation);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    if (!frame->has_value())
    {
        return std::vector<std::uint32_t>{};
    }
    StreamFrame s = ParseStreamFrame(frame->value());
    if (!s.ok)
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "MUT/DMA stream framing or checksum invalid");
    }
    if (s.data.size() != ResponseDataLength(channels_))
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse,
                             "MUT/DMA stream payload length does not match selected widths");
    }
    return DecodeStreamValues(channels_, s.data);
}
} // namespace mutdma
