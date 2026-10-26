#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>

namespace fastecu::logging
{
namespace
{
std::vector<mutdma::Channel> MakeWireChannels(const std::vector<LoggingChannel>& channels)
{
    return channels |
           std::views::transform(
               [](const LoggingChannel& channel)
               {
                   return mutdma::Channel{
                       .id = static_cast<std::uint16_t>(channel.address),
                       .len = static_cast<bytes::Byte>(channel.length),
                   };
               }) |
           std::ranges::to<std::vector<mutdma::Channel>>();
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
    if (auto ready = CheckLoggingRunCanContinue(*transport_, cancellation); !ready.has_value())
    {
        return ready;
    }
    return driver_.StartFreeFormLog(wire_channels_, 0xA0, 0xA1, cancellation);
}

fastecu::Result<PollData> MutDmaLoggingProtocol::Poll(std::chrono::milliseconds timeout,
                                                      const fastecu::ICancellationToken& cancellation)
{
    if (auto ready = CheckLoggingRunCanContinue(*transport_, cancellation); !ready.has_value())
    {
        return std::unexpected(ready.error());
    }
    if (!driver_.IsStreaming())
    {
        return PollData{.responded = false};
    }

    auto values = driver_.PollOnce(timeout, cancellation);
    if (!values.has_value())
    {
        return std::unexpected(values.error());
    }
    if (values->empty())
    {
        return PollData{.responded = false};
    }

    PollData data{.responded = true};
    const std::size_t sample_count = std::min(values->size(), channels_.size());
    data.samples = std::views::iota(std::size_t{0}, sample_count) |
                   std::views::transform(
                       [&](std::size_t i)
                       {
                           return ProtocolSample{
                               .channel_id = channels_[i].id,
                               .raw_value = std::to_string(values->at(i)),
                           };
                       }) |
                   std::ranges::to<std::vector<ProtocolSample>>();
    return data;
}

fastecu::Status MutDmaLoggingProtocol::Stop()
{
    return {};
}

} // namespace fastecu::logging
