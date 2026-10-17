#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mitsu_colt_can_cdbg
{

namespace
{
using namespace std::chrono_literals;

fastecu::Result<bytes::Bytes> SendAndReceive(cdbg::ICanTransport& transport, bytes::ByteView command,
                                             const fastecu::ICancellationToken& cancellation,
                                             std::string_view failure_detail)
{
    auto written = transport.Write(kRequestCanId, command);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != command.size())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "partial CAN write");
    }
    auto reply = transport.Read(250ms, cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value() || reply->value().id != kReplyCanId || reply->value().payload.empty())
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, std::string(failure_detail));
    }
    return std::move(reply->value().payload);
}

fastecu::Status Send(cdbg::ICanTransport& transport, bytes::ByteView command,
                     const fastecu::ICancellationToken& cancellation, std::string_view failure_detail)
{
    if (auto reply = SendAndReceive(transport, command, cancellation, failure_detail); !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return {};
}

// Opens a session and passes seed/key security access.
fastecu::Status Unlock(cdbg::ICanTransport& transport, const fastecu::ICancellationToken& cancellation)
{
    if (auto init = Send(transport, BuildInitFrame(), cancellation, "CDBG session init failed"); !init.has_value())
    {
        return init;
    }
    auto seed =
        SendAndReceive(transport, BuildSecuritySeedRequestFrame(), cancellation, "CDBG security seed request failed");
    if (!seed.has_value())
    {
        return std::unexpected(seed.error());
    }
    auto granted = SendAndReceive(transport, BuildSecurityKeyFrame(SeedToKey(ExtractSeed(*seed))), cancellation,
                                  "CDBG security key request failed");
    if (!granted.has_value())
    {
        return std::unexpected(granted.error());
    }
    if (!SecurityGranted(*granted))
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "CDBG security access denied");
    }
    return {};
}

// Resets log `instance` and defines each of its frames.
fastecu::Status ConfigureFrames(cdbg::ICanTransport& transport, bytes::Byte instance,
                                const std::vector<std::vector<CdbgChannel>>& frames,
                                const fastecu::ICancellationToken& cancellation)
{
    if (auto reset = Send(transport, BuildLogResetFrame(instance), cancellation, "CDBG log reset failed");
        !reset.has_value())
    {
        return reset;
    }
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        for (const CdbgFrame& cmd : BuildFrameInitFrames(instance, static_cast<bytes::Byte>(f), frames.at(f)))
        {
            if (auto setup = Send(transport, cmd, cancellation, "CDBG log frame setup failed"); !setup.has_value())
            {
                return setup;
            }
        }
    }
    return {};
}
} // namespace

fastecu::Status CdbgLogDriver::StartFreeFormLog(const std::vector<CdbgChannel>& channels, bytes::Byte instance,
                                                std::uint32_t interval_ms,
                                                const fastecu::ICancellationToken& cancellation)
{
    streaming_ = false;
    frames_.clear();

    if (channels.empty())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "no CDBG log parameters selected");
    }
    if (std::ranges::any_of(channels,
                            [](const CdbgChannel& ch) { return ch.size != 1 && ch.size != 2 && ch.size != 4; }))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "CDBG log parameter has unsupported byte length");
    }
    if (auto unlocked = Unlock(t_, cancellation); !unlocked.has_value())
    {
        return unlocked;
    }
    if (!BatchChannelsIntoFrames(channels, frames_))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "too many CDBG log parameters selected");
    }
    if (auto configured = ConfigureFrames(t_, instance, frames_, cancellation); !configured.has_value())
    {
        return configured;
    }
    if (auto started = Send(t_, BuildLogStartFrame(instance, static_cast<bytes::Byte>(frames_.size()), interval_ms),
                            cancellation, "CDBG log start failed");
        !started.has_value())
    {
        return started;
    }
    streaming_ = true;
    return {};
}

fastecu::Result<CdbgLogDriver::PollResult> CdbgLogDriver::PollOnce(std::chrono::milliseconds timeout,
                                                                   const fastecu::ICancellationToken& cancellation)
{
    if (!streaming_)
    {
        return PollResult{};
    }

    auto read = t_.Read(timeout, cancellation);
    if (!read.has_value())
    {
        return std::unexpected(read.error());
    }
    if (!read->has_value() || read->value().id != kReplyCanId || read->value().payload.empty())
    {
        return PollResult{};
    }
    const auto& frame = read->value().payload;
    const auto frame_index = static_cast<std::size_t>(frame.front());
    if (frame_index >= frames_.size())
    {
        return PollResult{};
    }
    auto decoded = DecodeFrame(static_cast<bytes::Byte>(frame_index), frames_[frame_index], frame);
    if (decoded.empty())
    {
        return PollResult{};
    }
    std::size_t offset = 0;
    for (std::size_t index = 0; index < frame_index; ++index)
    {
        offset += frames_[index].size();
    }
    return PollResult{.responded = true, .channel_offset = offset, .values = std::move(decoded)};
}

} // namespace mitsu_colt_can_cdbg
