#include "src/backend/logging/logging_session.h"
#include "src/backend/logging/logging_read_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
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
        // A8 + mode + three address bytes per channel must fit the SSM
        // one-byte payload-length field.
        return true; // Physical SSM validation follows with the owned read plan.
    case LoggingProtocolId::kMutDma:
        return channels.size() <= 255;
    case LoggingProtocolId::kCdbg:
    {
        std::vector<mitsu_colt_can_cdbg::CdbgChannel> wire_channels;
        wire_channels.reserve(channels.size());
        for (const LoggingChannel& channel : channels)
        {
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
                               std::optional<SsmReadPlan> read_plan,
                               std::vector<expression::CompiledExpression> conversions)
    : protocol_(protocol), channels_(std::move(channels)), policy_(policy), read_plan_(std::move(read_plan)),
      conversions_(std::move(conversions))
{
}

const std::optional<SsmReadPlan>& LoggingSession::ReadPlan() const
{
    return read_plan_;
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
    // #lizard forgives(cyclomatic_complexity)
    // TODO: Reduce complexity and remove forgiveness; see docs/tech-debt.md.
    if (!ValidProtocol(protocol))
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid logging protocol");
    }
    if (channel.id.empty())
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid id; expected a nonempty channel id");
    }
    if (channel.length != 1 && channel.length != 2 && channel.length != 4)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid length; expected 1, 2 or 4 bytes");
    }
    if (!ValidAddress(protocol, channel.address))
    {
        return fastecu::Fail(
            ErrorKind::kInvalidConfig,
            std::format("invalid address '{:x}'; outside the selected protocol's address range", channel.address));
    }
    if (channel.sample_bit.has_value() && (*channel.sample_bit >= 8 || channel.length != 1))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid switch sample bit; expected 0–7 in a one-byte source");
    }
    if ((channel.storage != NumericStorage::kUnsignedInteger && channel.storage != NumericStorage::kSignedInteger &&
         channel.storage != NumericStorage::kFloat) ||
        (channel.byte_order != NumericByteOrder::kBigEndian && channel.byte_order != NumericByteOrder::kLittleEndian) ||
        (channel.storage == NumericStorage::kFloat && channel.length != 4))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid numeric storage or byte order");
    }
    if (protocol == LoggingProtocolId::kSsm && channel.byte_addresses.empty() &&
        channel.length - 1 > 0xffffff - channel.address)
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
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid precision; expected 0–15 decimal places");
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
    if (channel.from_byte_expression.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid expression; expected a nonempty conversion");
    }
    const auto compiled = expression::Compile(channel.from_byte_expression);
    if (!compiled.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("invalid expression '{}': {}", channel.from_byte_expression, compiled.error().detail));
    }
    return {};
}

fastecu::Result<LoggingSession> MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                                                   LoggingPolicy policy)
{
    // #lizard forgives(cyclomatic_complexity)
    // TODO: Reduce complexity and remove forgiveness; see docs/tech-debt.md.
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
        if (!ids.insert(channel.id).second)
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "duplicate logging channel id");
        }
        if (const auto valid = ValidateChannelFields(protocol, channel); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        if (channel.from_byte_expression.empty())
        {
            return Fail(ErrorKind::kInvalidConfig, "empty logging conversion");
        }
        auto compiled = expression::Compile(channel.from_byte_expression);
        if (!compiled.has_value())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                                 std::format("invalid logging conversion: {}", compiled.error().detail));
        }
        conversions.push_back(std::move(*compiled));
    }

    if (!ValidWireShape(protocol, channels))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "logging channels do not fit the selected protocol");
    }

    if (protocol == LoggingProtocolId::kMutDma)
    {
        std::size_t bytes = 0;
        for (const auto& source : channels)
        {
            bytes += source.length;
        }
        if (channels.size() > 96 || bytes > 96)
        {
            return Fail(ErrorKind::kInvalidConfig, "MUT request exceeds 96 entries or 96 response bytes");
        }
    }
    std::optional<SsmReadPlan> read_plan;
    if (protocol == LoggingProtocolId::kSsm)
    {
        auto result = MakeSsmReadPlan(channels);
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        read_plan = std::move(*result);
    }
    return LoggingSession(protocol, std::move(channels), policy, std::move(read_plan), std::move(conversions));
}

} // namespace fastecu::logging
