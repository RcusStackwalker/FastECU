#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"

#include <algorithm>
#include <string>
#include <utility>

namespace fastecu::logging
{
namespace
{
std::vector<MitsuColtCanCdbg::CdbgChannel> makeWireChannels(
    const std::vector<LoggingChannel>& channels)
{
    std::vector<MitsuColtCanCdbg::CdbgChannel> wire_channels;
    wire_channels.reserve(channels.size());
    for (const LoggingChannel& channel : channels)
    {
        wire_channels.push_back(MitsuColtCanCdbg::CdbgChannel{
            .pointer = channel.address,
            .size = static_cast<bytes::Byte>(channel.length),
        });
    }
    return wire_channels;
}

fastecu::Status checkCancellation(const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled,
                             "CDBG logging cancelled");
    }
    return {};
}
} // namespace

CdbgLoggingProtocol::CdbgLoggingProtocol(
    std::unique_ptr<cdbg::ICanTransport> transport,
    std::vector<LoggingChannel> channels)
    : transport_(std::move(transport)),
      channels_(std::move(channels)),
      wire_channels_(makeWireChannels(channels_)),
      driver_(*transport_)
{
}

fastecu::Status CdbgLoggingProtocol::start(
    const fastecu::ICancellationToken& cancellation)
{
    if (auto status = checkCancellation(cancellation); !status)
    {
        return status;
    }
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected,
                             "adapter disconnected");
    }
    return driver_.startFreeFormLog(wire_channels_, 0, 10, cancellation);
}

fastecu::Result<PollData> CdbgLoggingProtocol::poll(
    int timeout_ms, const fastecu::ICancellationToken& cancellation)
{
    if (auto status = checkCancellation(cancellation); !status)
    {
        return std::unexpected(status.error());
    }
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected,
                             "adapter disconnected");
    }
    if (!driver_.isStreaming())
    {
        return PollData{.responded = false};
    }

    auto values = driver_.pollOnce(timeout_ms, cancellation);
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

fastecu::Status CdbgLoggingProtocol::stop()
{
    return {};
}

} // namespace fastecu::logging
