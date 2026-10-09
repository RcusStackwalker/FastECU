#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"

#include <string>
#include <string_view>
#include <utility>

namespace mitsu_colt_can_cdbg
{

namespace
{
using namespace std::chrono_literals;

fastecu::Result<bytes::Bytes> sendAndReceive(cdbg::ICanTransport& transport, bytes::ByteView command,
                                             const fastecu::ICancellationToken& cancellation,
                                             std::string_view failure_detail)
{
    auto written = transport.write(kRequestCanId, command);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    if (*written != command.size())
    {
        return fastecu::fail(fastecu::ErrorKind::kInternal, "partial CAN write");
    }
    auto reply = transport.read(250ms, cancellation);
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value() || reply->value().id != kReplyCanId || reply->value().payload.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::kBadResponse, std::string(failure_detail));
    }
    return std::move(reply->value().payload);
}
} // namespace

fastecu::Status CdbgLogDriver::startFreeFormLog(const std::vector<CdbgChannel>& channels, bytes::Byte instance,
                                                std::uint32_t interval_ms,
                                                const fastecu::ICancellationToken& cancellation)
{
    streaming_ = false;
    frames_.clear();
    last_values_.clear();

    if (channels.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::kInvalidConfig, "no CDBG log parameters selected");
    }

    for (const CdbgChannel& ch : channels)
    {
        if (ch.size != 1 && ch.size != 2 && ch.size != 4)
        {
            return fastecu::fail(fastecu::ErrorKind::kInvalidConfig, "CDBG log parameter has unsupported byte length");
        }
    }

    auto reply = sendAndReceive(t_, BuildInitFrame(), cancellation, "CDBG session init failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    reply = sendAndReceive(t_, BuildSecuritySeedRequestFrame(), cancellation, "CDBG security seed request failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    std::uint32_t key = SeedToKey(ExtractSeed(*reply));
    reply = sendAndReceive(t_, BuildSecurityKeyFrame(key), cancellation, "CDBG security key request failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }
    if (!SecurityGranted(*reply))
    {
        return fastecu::fail(fastecu::ErrorKind::kBadResponse, "CDBG security access denied");
    }

    if (!BatchChannelsIntoFrames(channels, frames_))
    {
        return fastecu::fail(fastecu::ErrorKind::kInvalidConfig, "too many CDBG log parameters selected");
    }

    reply = sendAndReceive(t_, BuildLogResetFrame(instance), cancellation, "CDBG log reset failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    for (std::size_t f = 0; f < frames_.size(); ++f)
    {
        const std::vector<CdbgFrame> cmds = BuildFrameInitFrames(instance, static_cast<bytes::Byte>(f), frames_.at(f));
        for (const CdbgFrame& cmd : cmds)
        {
            reply = sendAndReceive(t_, cmd, cancellation, "CDBG log frame setup failed");
            if (!reply)
            {
                return std::unexpected(reply.error());
            }
        }
    }

    reply = sendAndReceive(t_, BuildLogStartFrame(instance, static_cast<bytes::Byte>(frames_.size()), interval_ms),
                           cancellation, "CDBG log start failed");
    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    std::size_t total_channels = 0;
    for (const auto& frame : frames_)
    {
        total_channels += frame.size();
    }
    last_values_.assign(total_channels, 0);

    streaming_ = true;
    return {};
}

fastecu::Result<CdbgLogDriver::PollResult> CdbgLogDriver::pollOnce(std::chrono::milliseconds timeout,
                                                                   const fastecu::ICancellationToken& cancellation)
{
    if (!streaming_)
    {
        return PollResult{};
    }

    auto read = t_.read(timeout, cancellation);
    if (!read)
    {
        return std::unexpected(read.error());
    }
    bool decoded_response = false;
    if (read->has_value() && read->value().id == kReplyCanId && !read->value().payload.empty())
    {
        const bytes::Bytes& frame = read->value().payload;
        bytes::Byte frame_idx = frame.front();
        if (frame_idx < static_cast<bytes::Byte>(frames_.size()))
        {
            std::vector<std::uint32_t> decoded = DecodeFrame(frame_idx, frames_.at(frame_idx), frame);
            if (!decoded.empty())
            {
                decoded_response = true;
                std::size_t offset = 0;
                for (std::size_t f = 0; f < frame_idx; ++f)
                {
                    offset += frames_.at(f).size();
                }
                for (std::size_t i = 0; i < decoded.size(); ++i)
                {
                    last_values_[offset + i] = decoded.at(i);
                }
            }
        }
    }
    if (!decoded_response)
    {
        return PollResult{};
    }
    return PollResult{.responded = true, .values = last_values_};
}

} // namespace mitsu_colt_can_cdbg
