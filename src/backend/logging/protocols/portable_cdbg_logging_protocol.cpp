#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>

namespace fastecu::logging
{
namespace
{
std::vector<mitsu_colt_can_cdbg::CdbgChannel> MakeWireChannels(const std::vector<LoggingChannel>& channels)
{
    return channels |
           std::views::transform(
               [](const LoggingChannel& channel)
               {
                   return mitsu_colt_can_cdbg::CdbgChannel{
                       .pointer = channel.address,
                       .size = static_cast<bytes::Byte>(channel.length),
                   };
               }) |
           std::ranges::to<std::vector<mitsu_colt_can_cdbg::CdbgChannel>>();
}

} // namespace

CdbgLoggingProtocol::CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport,
                                         std::vector<LoggingChannel> channels)
    : transport_(std::move(transport)), channels_(std::move(channels)), wire_channels_(MakeWireChannels(channels_)),
      driver_(*transport_)
{
}

fastecu::Status CdbgLoggingProtocol::Start(const fastecu::ICancellationToken& cancellation)
{
    if (auto ready = CheckLoggingRunCanContinue(*transport_, cancellation); !ready.has_value())
    {
        return ready;
    }
    return driver_.StartFreeFormLog(wire_channels_, 0, 10, cancellation);
}

fastecu::Result<PollData> CdbgLoggingProtocol::Poll(std::chrono::milliseconds timeout,
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
    if (!values->responded)
    {
        return PollData{.responded = false};
    }

    PollData data{.responded = true};
    if (values->channel_offset > channels_.size() || values->values.size() > channels_.size() - values->channel_offset)
    {
        return Fail(ErrorKind::kInternal, "CDBG frame values lie outside the captured channels");
    }
    const std::size_t sample_count = values->values.size();
    data.samples = std::views::iota(std::size_t{0}, sample_count) |
                   std::views::transform(
                       [&](std::size_t i)
                       {
                           return ProtocolSample{
                               .channel_id = channels_[values->channel_offset + i].id,
                               .raw_value = std::to_string(values->values.at(i)),
                           };
                       }) |
                   std::ranges::to<std::vector<ProtocolSample>>();
    return data;
}

fastecu::Status CdbgLoggingProtocol::Stop()
{
    return {};
}

} // namespace fastecu::logging
