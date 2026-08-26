#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"

#include <string>
#include <string_view>
#include <utility>

namespace MitsuColtCanCdbg
{

namespace
{
fastecu::Result<bytes::Bytes> sendAndReceive(cdbg::ICanTransport& transport, const cdbg::CdbgProtocolConfig& config,
                                             bytes::ByteView command, const fastecu::ICancellationToken& cancellation,
                                             std::string_view failureDetail)
{
    auto written = transport.write(config.request_id(), command);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    if (*written != command.size())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "partial CAN write");
    }
    auto reply = transport.read(250, cancellation);
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value() || reply->value().id != config.reply_id() || reply->value().payload.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse, std::string(failureDetail));
    }
    return std::move(reply->value().payload);
}
} // namespace

CdbgLogDriver::CdbgLogDriver(cdbg::ICanTransport& transport, cdbg::CdbgProtocolConfig config)
    : transport_(transport), config_(std::move(config))
{
}

fastecu::Status CdbgLogDriver::startFreeFormLog(const std::vector<CdbgChannel>& channels,
                                                const fastecu::ICancellationToken& cancellation)
{
    streaming_ = false;
    frames_.clear();
    lastValues_.clear();

    if (channels.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "no CDBG log parameters selected");
    }

    for (const CdbgChannel& ch : channels)
    {
        if (ch.size != 1 && ch.size != 2 && ch.size != 4)
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "CDBG log parameter has unsupported byte length");
        }
    }

    auto reply = sendAndReceive(transport_, config_, buildInitFrame(), cancellation, "CDBG session init failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    reply = sendAndReceive(transport_, config_, buildSecuritySeedRequestFrame(), cancellation,
                           "CDBG security seed request failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    std::uint32_t key = seedToKey(extractSeed(*reply));
    reply = sendAndReceive(transport_, config_, buildSecurityKeyFrame(key), cancellation,
                           "CDBG security key request failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    if (!securityGranted(*reply))
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse, "CDBG security access denied");
    }

    if (!batchChannelsIntoFrames(channels, frames_))
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "too many CDBG log parameters selected");
    }

    reply = sendAndReceive(transport_, config_, buildLogResetFrame(config_.stream_instance()), cancellation,
                           "CDBG log reset failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    for (std::size_t f = 0; f < frames_.size(); ++f)
    {
        const std::vector<CdbgFrame> cmds =
            buildFrameInitFrames(config_.stream_instance(), static_cast<bytes::Byte>(f), frames_.at(f));
        for (const CdbgFrame& cmd : cmds)
        {
            reply = sendAndReceive(transport_, config_, cmd, cancellation, "CDBG log frame setup failed");
            if (!reply)
            {
                return std::unexpected(reply.error());
            }
        }
    }

    reply = sendAndReceive(transport_, config_,
                           buildLogStartFrame(config_.stream_instance(), static_cast<bytes::Byte>(frames_.size()),
                                              config_.sampling_interval_ms()),
                           cancellation, "CDBG log start failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    std::size_t totalChannels = 0;
    for (const auto& frame : frames_)
    {
        totalChannels += frame.size();
    }
    lastValues_.assign(totalChannels, 0);

    streaming_ = true;
    return {};
}

fastecu::Result<CdbgLogDriver::PollResult> CdbgLogDriver::pollOnce(int timeoutMs,
                                                                   const fastecu::ICancellationToken& cancellation)
{
    if (!streaming_)
    {
        return PollResult{};
    }

    auto read = transport_.read(timeoutMs, cancellation);
    if (!read)
    {
        return std::unexpected(read.error());
    }
    bool decoded_response = false;
    if (read->has_value() && read->value().id == config_.reply_id() && !read->value().payload.empty())
    {
        const bytes::Bytes& frame = read->value().payload;
        bytes::Byte frameIdx = frame.front();
        if (frameIdx < static_cast<bytes::Byte>(frames_.size()))
        {
            std::vector<std::uint32_t> decoded = decodeFrame(frameIdx, frames_.at(frameIdx), frame);
            if (!decoded.empty())
            {
                decoded_response = true;
                std::size_t offset = 0;
                for (std::size_t f = 0; f < frameIdx; ++f)
                {
                    offset += frames_.at(f).size();
                }
                for (std::size_t i = 0; i < decoded.size(); ++i)
                {
                    lastValues_[offset + i] = decoded.at(i);
                }
            }
        }
    }
    if (!decoded_response)
    {
        return PollResult{};
    }
    return PollResult{.responded = true, .values = lastValues_};
}

} // namespace MitsuColtCanCdbg
