#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"

#include <algorithm>
#include <string>
#include <utility>

namespace fastecu::logging
{
namespace
{
std::vector<mitsu_colt_can_cdbg::CdbgChannel> MakeWireChannels(const std::vector<LoggingChannel>& channels)
{
    std::vector<mitsu_colt_can_cdbg::CdbgChannel> wire_channels;
    wire_channels.reserve(channels.size());
    for (const LoggingChannel& channel : channels)
    {
        wire_channels.push_back(mitsu_colt_can_cdbg::CdbgChannel{
            .pointer = channel.address,
            .size = static_cast<bytes::Byte>(channel.length),
        });
    }
    return wire_channels;
}

fastecu::Status CheckCancellation(const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "CDBG logging cancelled");
    }
    return {};
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
    if (auto status = CheckCancellation(cancellation); !status)
    {
        return status;
    }
    if (!transport_->IsOpen())
    {
        return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "adapter disconnected");
    }
    return driver_.StartFreeFormLog(wire_channels_, 0, 10, cancellation);
}

fastecu::Result<PollData> CdbgLoggingProtocol::Poll(std::chrono::milliseconds timeout,
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
    if (!values->responded)
    {
        return PollData{.responded = false};
    }

    PollData data{.responded = true};
    const std::size_t sample_count = std::min(values->values.size(), channels_.size());
    data.samples.reserve(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i)
    {
        data.samples.push_back(ProtocolSample{
            .channel_id = channels_[i].id,
            .raw_value = std::to_string(values->values.at(i)),
        });
    }
    return data;
}

fastecu::Status CdbgLoggingProtocol::Stop()
{
    return {};
}

} // namespace fastecu::logging
