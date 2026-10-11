#include "src/backend/logging/logging_session.h"
#include "src/backend/logging/logging_read_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/algorithms/expression/expression.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

namespace fastecu::logging
{

using namespace std::chrono_literals;

namespace
{

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

bool ValidWireShape(LoggingProtocolId protocol, const std::vector<LoggingChannel>& channels)
{
    switch (protocol)
    {
    case LoggingProtocolId::kSsm:
        return MakeSsmReadPlan(channels).has_value();
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

LoggingSession::LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy,
                               std::vector<expression::CompiledExpression> conversions)
    : protocol_(protocol), channels_(std::move(channels)), policy_(policy), conversions_(std::move(conversions))
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

const expression::CompiledExpression *LoggingSession::FindConversion(std::string_view id) const
{
    for (std::size_t index = 0; index < channels_.size(); ++index)
    {
        if (channels_[index].id == id)
        {
            return &conversions_[index];
        }
    }
    return nullptr;
}

namespace
{
fastecu::Status ValidateChannelFields(LoggingProtocolId protocol, const LoggingChannel& channel)
{
    if (!ValidProtocol(protocol))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid logging protocol");
    }
    if (channel.id.empty())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "missing channel id");
    }
    if (channel.length != 1 && channel.length != 2 && channel.length != 4)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid length; expected 1, 2 or 4 bytes");
    }
    if (!ValidAddress(protocol, channel.address))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "address exceeds the selected protocol's range");
    }
    if ((channel.storage != NumericStorage::kUnsignedInteger && channel.storage != NumericStorage::kSignedInteger &&
         channel.storage != NumericStorage::kFloat) ||
        (channel.byte_order != NumericByteOrder::kBigEndian && channel.byte_order != NumericByteOrder::kLittleEndian) ||
        (channel.storage == NumericStorage::kFloat && channel.length != 4))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid numeric storage or byte order");
    }
    if (protocol == LoggingProtocolId::kSsm && channel.length - 1 > 0xffffff - channel.address)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "expanded byte address exceeds the SSM range");
    }
    if (protocol != LoggingProtocolId::kSsm &&
        (channel.storage != NumericStorage::kUnsignedInteger || channel.byte_order != NumericByteOrder::kBigEndian))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                             "numeric metadata override is unsupported by this protocol");
    }
    if (channel.decimal_precision > 15)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                             "invalid format precision; expected 0–15 decimal places");
    }
    return {};
}
} // namespace

fastecu::Status ValidateLoggingChannel(LoggingProtocolId protocol, const LoggingChannel& channel)
{
    const auto fields = ValidateChannelFields(protocol, channel);
    if (!fields.has_value())
    {
        return fields;
    }
    if (channel.from_byte_expression.empty() || !expression::Compile(channel.from_byte_expression).has_value())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                             "invalid expression; provide a finite numeric conversion");
    }
    return {};
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

    std::vector<expression::CompiledExpression> conversions;
    conversions.reserve(channels.size());
    std::unordered_set<std::string> ids;
    for (const LoggingChannel& channel : channels)
    {
        if (!ids.insert(channel.id).second || !ValidateChannelFields(protocol, channel).has_value() ||
            channel.from_byte_expression.empty())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid logging channel");
        }
        auto compiled = expression::Compile(channel.from_byte_expression);
        if (!compiled.has_value())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                                 "invalid logging conversion: " + compiled.error().detail);
        }
        conversions.push_back(std::move(*compiled));
    }

    if (!ValidWireShape(protocol, channels))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "logging channels do not fit the selected protocol");
    }

    return LoggingSession(protocol, std::move(channels), policy, std::move(conversions));
}

} // namespace fastecu::logging
