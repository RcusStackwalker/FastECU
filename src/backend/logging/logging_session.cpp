#include "src/backend/logging/logging_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/algorithms/expression/checked_expression.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

namespace fastecu::logging
{

using namespace std::chrono_literals;

namespace
{

bool ValidExpression(const LoggingChannel& channel)
{
    if (channel.from_byte_expression.empty())
    {
        return false;
    }
    // A syntax error fails every probe; a probe-dependent failure such as a
    // division by x-1 only rejects the expression if no probe evaluates.
    const auto evaluates = [&channel](double probe)
    { return fastecu::expression::EvaluateChecked(channel.from_byte_expression, probe).has_value(); };
    constexpr std::array<double, 3> kProbes{1.0, 16.0, 1616.0};
    return std::ranges::any_of(kProbes, evaluates);
}

bool ValidAddress(LoggingProtocolId protocol, std::uint32_t address)
{
    switch (protocol)
    {
    case LoggingProtocolId::kSsm:
        return address <= 0x00ffffff;
    case LoggingProtocolId::kMutDma:
        return address <= 0x0000ffff;
    case LoggingProtocolId::kCdbg:
        return true;
    }
    return false;
}

bool ValidProtocol(LoggingProtocolId protocol)
{
    return protocol == LoggingProtocolId::kSsm || protocol == LoggingProtocolId::kMutDma ||
           protocol == LoggingProtocolId::kCdbg;
}

bool ValidRawAssembly(RawAssembly raw_assembly)
{
    return raw_assembly == RawAssembly::kDecimalBytesConcatenated ||
           raw_assembly == RawAssembly::kUnsignedIntegerDecimal;
}

bool ValidWireShape(LoggingProtocolId protocol, const std::vector<LoggingChannel>& channels)
{
    switch (protocol)
    {
    case LoggingProtocolId::kSsm:
        // A8 + mode + three address bytes per channel must fit the SSM
        // one-byte payload-length field.
        return channels.size() <= 84;
    case LoggingProtocolId::kMutDma:
        if (channels.size() > 255)
        {
            return false;
        }
        return std::all_of(channels.begin(), channels.end(), [](const LoggingChannel& channel)
                           { return channel.length == 1 || channel.length == 2 || channel.length == 4; });
    case LoggingProtocolId::kCdbg:
    {
        std::vector<mitsu_colt_can_cdbg::CdbgChannel> wire_channels;
        wire_channels.reserve(channels.size());
        for (const LoggingChannel& channel : channels)
        {
            if (channel.length != 1 && channel.length != 2 && channel.length != 4)
            {
                return false;
            }
            wire_channels.push_back({channel.address, static_cast<bytes::Byte>(channel.length)});
        }
        std::vector<std::vector<mitsu_colt_can_cdbg::CdbgChannel>> frames;
        return mitsu_colt_can_cdbg::BatchChannelsIntoFrames(wire_channels, frames);
    }
    }
    return false;
}

} // namespace

LoggingSession::LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy)
    : protocol_(protocol), channels_(std::move(channels)), policy_(policy)
{
}

LoggingProtocolId LoggingSession::Protocol() const
{
    return protocol_;
}

const std::vector<LoggingChannel>& LoggingSession::Channels() const
{
    return channels_;
}

const LoggingPolicy& LoggingSession::Policy() const
{
    return policy_;
}

const LoggingChannel *LoggingSession::FindChannel(std::string_view id) const
{
    for (const LoggingChannel& channel : channels_)
    {
        if (channel.id == id)
        {
            return &channel;
        }
    }
    return nullptr;
}

fastecu::Result<LoggingSession> MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                                                   LoggingPolicy policy)
{
    if (!ValidProtocol(protocol) || policy.poll_timeout <= 0ms || policy.car_silence_miss_threshold <= 0 ||
        policy.reconnect_attempt_threshold <= 0 || policy.reconnect_retry_period < 0)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid logging policy");
    }
    if (protocol == LoggingProtocolId::kCdbg && channels.empty())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "no CDBG log parameters selected");
    }

    std::unordered_set<std::string> ids;
    for (const LoggingChannel& channel : channels)
    {
        if (channel.id.empty() || !ids.insert(channel.id).second || channel.length == 0 || channel.length > 255 ||
            !ValidAddress(protocol, channel.address) || !ValidRawAssembly(channel.raw_assembly) ||
            channel.decimal_precision > 15 || !ValidExpression(channel))
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid logging channel");
        }
    }

    if (!ValidWireShape(protocol, channels))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "logging channels do not fit the selected protocol");
    }

    return LoggingSession(protocol, std::move(channels), policy);
}

} // namespace fastecu::logging
