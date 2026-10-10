#include "src/backend/logging/logging_session.h"

#include <algorithm>
#include <array>
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
                               std::optional<SsmReadPlan> read_plan, mutdma::FreeformDialect dialect)
    : protocol_(protocol), channels_(std::move(channels)), policy_(policy), dialect_(dialect),
      read_plan_(std::move(read_plan))
{
}

const std::optional<SsmReadPlan>& LoggingSession::ReadPlan() const
{
    return read_plan_;
}

mutdma::FreeformDialect LoggingSession::MutDmaDialect() const
{
    return dialect_;
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

fastecu::Status ValidateLoggingChannel(LoggingProtocolId protocol, const LoggingChannel& channel)
{
    if (!ValidProtocol(protocol))
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid logging protocol");
    }
    if (channel.id.empty())
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid id; expected a nonempty channel id");
    }
    if (channel.length == 0 || channel.length > 255 ||
        (protocol != LoggingProtocolId::kSsm && channel.length != 1 && channel.length != 2 && channel.length != 4))
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig,
                             std::format("invalid length '{}'; expected {}", channel.length,
                                         protocol == LoggingProtocolId::kSsm ? "1–255 bytes" : "1, 2, or 4 bytes"));
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
    if (!ValidRawAssembly(channel.raw_assembly))
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid raw assembly");
    }
    if (channel.decimal_precision > 15)
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid precision; expected 0–15 decimal places");
    }
    if (!ValidExpression(channel))
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig,
                             std::format("invalid expression '{}'; expected a valid finite conversion expression",
                                         channel.from_byte_expression));
    }
    return {};
}

fastecu::Result<LoggingSession> MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                                                   LoggingPolicy policy, mutdma::FreeformDialect dialect)
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
        if (!ids.insert(channel.id).second)
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "duplicate logging channel id");
        }
        if (const auto valid = ValidateLoggingChannel(protocol, channel); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
    }

    if (!ValidWireShape(protocol, channels))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "logging channels do not fit the selected protocol");
    }

    if (dialect != mutdma::FreeformDialect::kLegacyBe && dialect != mutdma::FreeformDialect::kOem33520003)
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid MUT wire dialect");
    }
    if (protocol == LoggingProtocolId::kMutDma && dialect == mutdma::FreeformDialect::kOem33520003)
    {
        std::size_t bytes = 0;
        for (const auto& source : channels)
        {
            bytes += source.length;
        }
        if (channels.size() > 96 || bytes > 96)
        {
            return Fail(ErrorKind::kInvalidConfig, "oem-33520003 MUT request exceeds 96 entries or 96 response bytes");
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
    return LoggingSession(protocol, std::move(channels), policy, std::move(read_plan), dialect);
}

} // namespace fastecu::logging
