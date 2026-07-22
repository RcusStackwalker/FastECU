#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"

#include <string>
#include <string_view>
#include <utility>

namespace MitsuColtCanCdbg
{

namespace
{
fastecu::Result<bytes::Bytes> sendAndReceive(
    cdbg::ICanTransport& transport, bytes::ByteView command,
    const fastecu::ICancellationToken& cancellation, std::string_view failureDetail)
{
    auto written = transport.write(kRequestCanId, command);
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
    if (!reply->has_value() || reply->value().id != kReplyCanId ||
        reply->value().payload.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse, std::string(failureDetail));
    }
    return std::move(reply->value().payload);
}
} // namespace

fastecu::Status CdbgLogDriver::startFreeFormLog(
    const std::vector<CdbgChannel>& channels, bytes::Byte instance,
    std::uint32_t intervalMs, const fastecu::ICancellationToken& cancellation)
{
    streaming_ = false;
    frames_.clear();
    lastValues_.clear();

    if (channels.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "no CDBG log parameters selected");
    }

    for (const CdbgChannel& ch : channels)
    {
        if (ch.size != 1 && ch.size != 2 && ch.size != 4)
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "CDBG log parameter has unsupported byte length");
        }
    }

    auto reply = sendAndReceive(t_, buildInitFrame(), cancellation,
                                "CDBG session init failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    reply = sendAndReceive(t_, buildSecuritySeedRequestFrame(), cancellation,
                           "CDBG security seed request failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    std::uint32_t key = seedToKey(extractSeed(*reply));
    reply = sendAndReceive(t_, buildSecurityKeyFrame(key), cancellation,
                           "CDBG security key request failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    if (!securityGranted(*reply))
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse,
                             "CDBG security access denied");
    }

    if (!batchChannelsIntoFrames(channels, frames_))
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "too many CDBG log parameters selected");
    }

    reply = sendAndReceive(t_, buildLogResetFrame(instance), cancellation,
                           "CDBG log reset failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    for (std::size_t f = 0; f < frames_.size(); ++f)
    {
        const std::vector<CdbgFrame> cmds = buildFrameInitFrames(
            instance, static_cast<bytes::Byte>(f), frames_.at(f));
        for (const CdbgFrame& cmd : cmds)
        {
            reply = sendAndReceive(t_, cmd, cancellation,
                                   "CDBG log frame setup failed");
            if (!reply)
            {
                return std::unexpected(reply.error());
            }
        }
    }

    reply = sendAndReceive(
        t_, buildLogStartFrame(instance, static_cast<bytes::Byte>(frames_.size()), intervalMs),
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

fastecu::Result<std::vector<std::uint32_t>> CdbgLogDriver::pollOnce(
    int timeoutMs, const fastecu::ICancellationToken& cancellation)
{
    if (!streaming_)
    {
        return std::vector<std::uint32_t>{};
    }

    auto read = t_.read(timeoutMs, cancellation);
    if (!read)
    {
        return std::unexpected(read.error());
    }
    if (read->has_value() && read->value().id == kReplyCanId &&
        !read->value().payload.empty())
    {
        const bytes::Bytes& frame = read->value().payload;
        bytes::Byte frameIdx = frame.front();
        if (frameIdx < static_cast<bytes::Byte>(frames_.size()))
        {
            std::vector<std::uint32_t> decoded = decodeFrame(frameIdx, frames_.at(frameIdx), frame);
            if (!decoded.empty())
            {
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
    return lastValues_;
}

} // namespace MitsuColtCanCdbg
