#include "src/backend/logging/logging_session.h"
#include "src/backend/logging/logging_read_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
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
        std::ranges::transform(
            channels, std::back_inserter(wire_channels), [](const LoggingChannel& channel)
            { return mitsu_colt_can_cdbg::CdbgChannel{channel.address, static_cast<bytes::Byte>(channel.length)}; });
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
bool ValidLength(std::size_t length)
{
    return length == 1 || length == 2 || length == 4;
}

bool KnownEncoding(const LoggingChannel& channel)
{
    const bool storage = channel.storage == NumericStorage::kUnsignedInteger ||
                         channel.storage == NumericStorage::kSignedInteger || channel.storage == NumericStorage::kFloat;
    const bool byte_order =
        channel.byte_order == NumericByteOrder::kBigEndian || channel.byte_order == NumericByteOrder::kLittleEndian;
    return storage && byte_order && (channel.storage != NumericStorage::kFloat || channel.length == 4);
}

// Only SSM decodes anything other than big-endian unsigned integers.
bool OverridesEncoding(const LoggingChannel& channel)
{
    return channel.storage != NumericStorage::kUnsignedInteger || channel.byte_order != NumericByteOrder::kBigEndian;
}

// SSM expands a channel without explicit byte addresses into consecutive bytes.
bool ExceedsSsmRange(LoggingProtocolId protocol, const LoggingChannel& channel)
{
    return protocol == LoggingProtocolId::kSsm && channel.byte_addresses.empty() &&
           channel.length - 1 > 0xffffff - channel.address;
}

fastecu::Status ValidateChannelFields(LoggingProtocolId protocol, const LoggingChannel& channel)
{
    if (!ValidProtocol(protocol))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid logging protocol");
    }
    if (channel.id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid id; expected a nonempty channel id");
    }
    if (!ValidLength(channel.length))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid length; expected 1, 2 or 4 bytes");
    }
    if (!ValidAddress(protocol, channel.address))
    {
        return Fail(
            ErrorKind::kInvalidConfig,
            std::format("invalid address '{:x}'; outside the selected protocol's address range", channel.address));
    }
    if (channel.sample_bit.has_value() && (*channel.sample_bit >= 8 || channel.length != 1))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid switch sample bit; expected 0–7 in a one-byte source");
    }
    if (!KnownEncoding(channel))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid numeric storage or byte order");
    }
    if (ExceedsSsmRange(protocol, channel))
    {
        return Fail(ErrorKind::kInvalidConfig, "expanded byte address exceeds the SSM range");
    }
    if (protocol != LoggingProtocolId::kSsm && OverridesEncoding(channel))
    {
        return Fail(ErrorKind::kInvalidConfig, "numeric metadata override is unsupported by this protocol");
    }
    if (channel.decimal_precision > 15)
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid precision; expected 0–15 decimal places");
    }
    return {};
}

fastecu::Result<expression::CompiledExpression> CompileChannel(LoggingProtocolId protocol,
                                                               const LoggingChannel& channel)
{
    if (const auto fields = ValidateChannelFields(protocol, channel); !fields.has_value())
    {
        return std::unexpected(fields.error());
    }
    if (channel.from_byte_expression.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid expression; expected a nonempty conversion");
    }
    auto compiled = expression::Compile(channel.from_byte_expression);
    if (!compiled.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("invalid expression '{}': {}", channel.from_byte_expression, compiled.error().detail));
    }
    return *std::move(compiled);
}

bool ValidPolicy(const LoggingPolicy& policy)
{
    return policy.poll_timeout > 0ms && policy.car_silence_miss_threshold > 0 &&
           policy.reconnect_attempt_threshold > 0 && policy.reconnect_retry_period >= 0;
}

fastecu::Result<std::vector<expression::CompiledExpression>>
CompileChannels(LoggingProtocolId protocol, const std::vector<LoggingChannel>& channels)
{
    std::vector<expression::CompiledExpression> conversions;
    conversions.reserve(channels.size());
    std::unordered_set<std::string> ids;
    for (const LoggingChannel& channel : channels)
    {
        if (!ids.insert(channel.id).second)
        {
            return Fail(ErrorKind::kInvalidConfig, "duplicate logging channel id");
        }
        auto compiled = CompileChannel(protocol, channel);
        if (!compiled.has_value())
        {
            return std::unexpected(compiled.error());
        }
        conversions.push_back(std::move(*compiled));
    }
    return conversions;
}

fastecu::Status ValidateMutRequestSize(const std::vector<LoggingChannel>& channels)
{
    const std::size_t bytes = std::ranges::fold_left(channels, std::size_t{0}, [](std::size_t total, const auto& source)
                                                     { return total + source.length; });
    if (channels.size() > 96 || bytes > 96)
    {
        return Fail(ErrorKind::kInvalidConfig, "MUT request exceeds 96 entries or 96 response bytes");
    }
    return {};
}
} // namespace

fastecu::Status ValidateLoggingChannel(LoggingProtocolId protocol, const LoggingChannel& channel)
{
    if (auto compiled = CompileChannel(protocol, channel); !compiled.has_value())
    {
        return std::unexpected(compiled.error());
    }
    return {};
}

fastecu::Result<LoggingSession> MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                                                   LoggingPolicy policy)
{
    if (!ValidProtocol(protocol) || !ValidPolicy(policy))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid logging policy");
    }
    if (protocol == LoggingProtocolId::kCdbg && channels.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "no CDBG log parameters selected");
    }
    auto conversions = CompileChannels(protocol, channels);
    if (!conversions.has_value())
    {
        return std::unexpected(conversions.error());
    }
    if (!ValidWireShape(protocol, channels))
    {
        return Fail(ErrorKind::kInvalidConfig, "logging channels do not fit the selected protocol");
    }
    if (protocol == LoggingProtocolId::kMutDma)
    {
        if (auto size = ValidateMutRequestSize(channels); !size.has_value())
        {
            return std::unexpected(size.error());
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
    return LoggingSession(protocol, std::move(channels), policy, std::move(read_plan), std::move(*conversions));
}

} // namespace fastecu::logging
