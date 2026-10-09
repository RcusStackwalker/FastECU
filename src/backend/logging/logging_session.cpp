#include "src/backend/logging/logging_session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/algorithms/expression/expression_evaluator.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

namespace fastecu::logging
{

using namespace std::chrono_literals;

namespace
{

class ExpressionValidator
{
  public:
    explicit ExpressionValidator(std::string_view expression) : expression_(expression)
    {
    }

    bool Valid()
    {
        SkipSpaces();
        if (const ParsedValue value = ParseExpression(); !value.valid)
        {
            return false;
        }
        SkipSpaces();
        return position_ == expression_.size();
    }

  private:
    struct ParsedValue
    {
        bool valid = false;
        bool depends_on_x = false;
        double value = 0.0;
    };

    ParsedValue ParseExpression()
    {
        ParsedValue left = ParseTerm();
        if (!left.valid)
        {
            return {};
        }
        while (true)
        {
            char operation = '\0';
            if (Consume('+'))
            {
                operation = '+';
            }
            else if (Consume('-'))
            {
                operation = '-';
            }
            else
            {
                return left;
            }

            ParsedValue right = ParseTerm();
            if (!right.valid)
            {
                return {};
            }
            left = Combine(left, right, operation);
            if (!left.valid)
            {
                return {};
            }
        }
    }

    ParsedValue ParseTerm()
    {
        ParsedValue left = ParseFactor();
        if (!left.valid)
        {
            return {};
        }
        while (true)
        {
            char operation = '\0';
            if (Consume('*'))
            {
                operation = '*';
            }
            else if (Consume('/'))
            {
                operation = '/';
            }
            else
            {
                return left;
            }

            ParsedValue right = ParseFactor();
            if (!right.valid)
            {
                return {};
            }
            left = Combine(left, right, operation);
            if (!left.valid)
            {
                return {};
            }
        }
    }

    ParsedValue ParseFactor()
    {
        if (Consume('-'))
        {
            if (Consume('x'))
            {
                return {.valid = true, .depends_on_x = true};
            }
            ParsedValue number = ParseNumber();
            if (number.valid)
            {
                number.value = -number.value;
            }
            return number;
        }
        if (Consume('x'))
        {
            return {.valid = true, .depends_on_x = true};
        }
        if (Consume('('))
        {
            ParsedValue nested = ParseExpression();
            if (!nested.valid || !Consume(')'))
            {
                return {};
            }
            return nested;
        }
        return ParseNumber();
    }

    ParsedValue ParseNumber()
    {
        SkipSpaces();
        const std::size_t start = position_;
        bool saw_digit = false;
        bool saw_decimal_point = false;
        while (position_ < expression_.size())
        {
            const char current = expression_[position_];
            if (current >= '0' && current <= '9')
            {
                saw_digit = true;
                ++position_;
            }
            else if (current == '.' && !saw_decimal_point)
            {
                saw_decimal_point = true;
                ++position_;
            }
            else
            {
                break;
            }
        }
        if (!saw_digit || position_ == start)
        {
            return {};
        }
        try
        {
            // TODO: rewrite to use std::from_chars and analyze std::from_chars_result instead of try/catch
            const double value = std::stod(std::string(expression_.substr(start, position_ - start)));
            return std::isfinite(value) ? ParsedValue{.valid = true, .value = value} : ParsedValue{};
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    ParsedValue Combine(ParsedValue left, ParsedValue right, char operation) const
    {
        if (operation == '/' && !right.depends_on_x && right.value == 0.0)
        {
            return {};
        }
        if (left.depends_on_x || right.depends_on_x)
        {
            return {.valid = true, .depends_on_x = true};
        }

        switch (operation)
        {
        case '+':
            left.value += right.value;
            break;
        case '-':
            left.value -= right.value;
            break;
        case '*':
            left.value *= right.value;
            break;
        case '/':
            left.value /= right.value;
            break;
        default:
            return {};
        }
        return std::isfinite(left.value) ? left : ParsedValue{};
    }

    bool Consume(char expected)
    {
        SkipSpaces();
        if (position_ == expression_.size() || expression_[position_] != expected)
        {
            return false;
        }
        ++position_;
        return true;
    }

    void SkipSpaces()
    {
        while (position_ < expression_.size() && std::isspace(static_cast<unsigned char>(expression_[position_])))
        {
            ++position_;
        }
    }

    std::string_view expression_;
    std::size_t position_ = 0;
};

bool ValidExpression(const LoggingChannel& channel)
{
    if (channel.from_byte_expression.empty() || !ExpressionValidator(channel.from_byte_expression).Valid())
    {
        return false;
    }
    const auto is_finite = [&channel](const std::string_view& probe)
    {
        return std::isfinite(
            ExpressionEvaluate(channel.from_byte_expression, probe, static_cast<int>(channel.decimal_precision)));
    };
    constexpr std::array<std::string_view, 3> kProbes{"1", "16", "1616"};
    return std::ranges::any_of(kProbes, is_finite);
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
