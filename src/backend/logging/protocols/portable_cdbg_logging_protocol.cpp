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
    return CheckLoggingRunCanContinue(*transport_, cancellation)
        .and_then([&] { return driver_.StartFreeFormLog(wire_channels_, 0, 10, cancellation); });
}

fastecu::Result<PollData> CdbgLoggingProtocol::Poll(std::chrono::milliseconds timeout,
                                                    const fastecu::ICancellationToken& cancellation)
{
    return CheckLoggingRunCanContinue(*transport_, cancellation)
        .and_then(
            [&]() -> fastecu::Result<PollData>
            {
                if (!driver_.IsStreaming())
                {
                    return PollData{.responded = false};
                }
                return driver_.PollOnce(timeout, cancellation)
                    .and_then(
                        [&](const mitsu_colt_can_cdbg::CdbgLogDriver::PollResult& values) -> fastecu::Result<PollData>
                        {
                            if (!values.responded)
                            {
                                return PollData{.responded = false};
                            }

                            PollData data{.responded = true};
                            // Equivalent as `>=`: the driver's offset sums earlier frames, so it stays below the count.
                            // mull-ignore-next: cxx_gt_to_ge
                            if (values.channel_offset > channels_.size() ||
                                // Equivalent as `+`: a frame's values never reach past the channels after it.
                                // mull-ignore-next: cxx_sub_to_add
                                values.values.size() > channels_.size() - values.channel_offset)
                            {
                                return Fail(ErrorKind::kInternal,
                                            "CDBG frame values lie outside the captured channels");
                            }
                            const std::size_t sample_count = values.values.size();
                            data.samples = std::views::iota(std::size_t{0}, sample_count) |
                                           std::views::transform(
                                               [&](std::size_t i)
                                               {
                                                   return ProtocolSample{
                                                       .channel_id = channels_[values.channel_offset + i].id,
                                                       .raw_value = std::to_string(values.values.at(i)),
                                                   };
                                               }) |
                                           std::ranges::to<std::vector<ProtocolSample>>();
                            return data;
                        });
            });
}

fastecu::Status CdbgLoggingProtocol::Stop()
{
    return {};
}

} // namespace fastecu::logging
