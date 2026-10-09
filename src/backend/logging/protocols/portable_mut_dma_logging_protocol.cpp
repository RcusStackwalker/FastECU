#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace fastecu::logging
{
namespace
{
std::vector<mutdma::Channel> MakeWireChannels(const std::vector<LoggingChannel>& channels)
{
    std::vector<mutdma::Channel> wire_channels;
    wire_channels.reserve(channels.size());
    for (const LoggingChannel& channel : channels)
    {
        wire_channels.push_back(mutdma::Channel{
            .id = static_cast<std::uint16_t>(channel.address),
            .len = static_cast<bytes::Byte>(channel.length),
        });
    }
    return wire_channels;
}

fastecu::Status CheckCancellation(const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "MUT/DMA logging cancelled");
    }
    return {};
}
} // namespace

MutDmaLoggingProtocol::MutDmaLoggingProtocol(std::unique_ptr<mutdma::IKlineTransport> transport,
                                             std::unique_ptr<mutdma::IMutDmaInit> init,
                                             std::vector<LoggingChannel> channels)
    : transport_(std::move(transport)), init_(std::move(init)), channels_(std::move(channels)),
      wire_channels_(MakeWireChannels(channels_)), driver_(*transport_, *init_)
{
}

fastecu::Status MutDmaLoggingProtocol::Start(const fastecu::ICancellationToken& cancellation)
{
    if (auto status = CheckCancellation(cancellation); !status)
    {
        return status;
    }
    if (!transport_->IsOpen())
    {
        return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "adapter disconnected");
    }
    return driver_.StartFreeFormLog(wire_channels_, 0xA0, 0xA1, cancellation);
}

fastecu::Result<PollData> MutDmaLoggingProtocol::Poll(std::chrono::milliseconds timeout,
                                                      const fastecu::ICancellationToken& cancellation)
{
    if (auto status = CheckCancellation(cancellation); !status)
    {
        return std::unexpected(status.error());
    }
    if (!transport_->IsOpen())
    {
        return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "adapter disconnected");
    }
    if (!driver_.IsStreaming())
    {
        return PollData{.responded = false};
    }

    auto values = driver_.PollOnce(timeout, cancellation);
    if (!values)
    {
        return std::unexpected(values.error());
    }
    if (values->empty())
    {
        return PollData{.responded = false};
    }

    PollData data{.responded = true};
    const std::size_t sample_count = std::min(values->size(), channels_.size());
    data.samples.reserve(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i)
    {
        data.samples.push_back(ProtocolSample{
            .channel_id = channels_[i].id,
            .raw_value = std::to_string(values->at(i)),
        });
    }
    return data;
}

fastecu::Status MutDmaLoggingProtocol::Stop()
{
    return {};
}

} // namespace fastecu::logging
