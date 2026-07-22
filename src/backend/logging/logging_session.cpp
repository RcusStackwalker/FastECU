#include "src/backend/logging/logging_session.h"

#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/algorithms/expression/expression_evaluator.h"

namespace
{

class ExpressionValidator
{
  public:
    explicit ExpressionValidator(std::string_view expression) : expression_(expression)
    {
    }

    bool valid()
    {
        skip_spaces();
        const ParsedValue value = parse_expression();
        if (!value.valid)
        {
            return false;
        }
        skip_spaces();
        return position_ == expression_.size();
    }

  private:
    struct ParsedValue
    {
        bool valid = false;
        bool depends_on_x = false;
        double value = 0.0;
    };

    ParsedValue parse_expression()
    {
        ParsedValue left = parse_term();
        if (!left.valid)
        {
            return {};
        }
        while (true)
        {
            char operation = '\0';
            if (consume('+'))
            {
                operation = '+';
            }
            else if (consume('-'))
            {
                operation = '-';
            }
            else
            {
                return left;
            }

            ParsedValue right = parse_term();
            if (!right.valid)
            {
                return {};
            }
            left = combine(left, right, operation);
            if (!left.valid)
            {
                return {};
            }
        }
    }

    ParsedValue parse_term()
    {
        ParsedValue left = parse_factor();
        if (!left.valid)
        {
            return {};
        }
        while (true)
        {
            char operation = '\0';
            if (consume('*'))
            {
                operation = '*';
            }
            else if (consume('/'))
            {
                operation = '/';
            }
            else
            {
                return left;
            }

            ParsedValue right = parse_factor();
            if (!right.valid)
            {
                return {};
            }
            left = combine(left, right, operation);
            if (!left.valid)
            {
                return {};
            }
        }
    }

    ParsedValue parse_factor()
    {
        if (consume('-'))
        {
            if (consume('x'))
            {
                return {.valid = true, .depends_on_x = true};
            }
            ParsedValue number = parse_number();
            if (number.valid)
            {
                number.value = -number.value;
            }
            return number;
        }
        if (consume('x'))
        {
            return {.valid = true, .depends_on_x = true};
        }
        if (consume('('))
        {
            ParsedValue nested = parse_expression();
            if (!nested.valid || !consume(')'))
            {
                return {};
            }
            return nested;
        }
        return parse_number();
    }

    ParsedValue parse_number()
    {
        skip_spaces();
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
            const double value = std::stod(std::string(expression_.substr(start, position_ - start)));
            return std::isfinite(value) ? ParsedValue{.valid = true, .value = value} : ParsedValue{};
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    ParsedValue combine(ParsedValue left, ParsedValue right, char operation) const
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

    bool consume(char expected)
    {
        skip_spaces();
        if (position_ == expression_.size() || expression_[position_] != expected)
        {
            return false;
        }
        ++position_;
        return true;
    }

    void skip_spaces()
    {
        while (position_ < expression_.size() &&
               std::isspace(static_cast<unsigned char>(expression_[position_])))
        {
            ++position_;
        }
    }

    std::string_view expression_;
    std::size_t position_ = 0;
};

bool valid_expression(const LoggingChannel& channel)
{
    if (channel.from_byte_expression.empty() ||
        !ExpressionValidator(channel.from_byte_expression).valid())
    {
        return false;
    }
    constexpr std::array<std::string_view, 3> probes{"1", "16", "1616"};
    for (std::string_view probe : probes)
    {
        if (std::isfinite(expression_evaluate(
                channel.from_byte_expression, probe, static_cast<int>(channel.decimal_precision))))
        {
            return true;
        }
    }
    return false;
}

bool valid_address(LoggingProtocolId protocol, std::uint32_t address)
{
    switch (protocol)
    {
    case LoggingProtocolId::Ssm:
        return address <= 0x00ffffff;
    case LoggingProtocolId::MutDma:
        return address <= 0x0000ffff;
    case LoggingProtocolId::Cdbg:
        return true;
    }
    return false;
}

bool valid_protocol(LoggingProtocolId protocol)
{
    return protocol == LoggingProtocolId::Ssm || protocol == LoggingProtocolId::MutDma ||
           protocol == LoggingProtocolId::Cdbg;
}

bool valid_raw_assembly(RawAssembly raw_assembly)
{
    return raw_assembly == RawAssembly::DecimalBytesConcatenated ||
           raw_assembly == RawAssembly::UnsignedIntegerDecimal;
}

} // namespace

LoggingSession::LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                               LoggingPolicy policy)
    : protocol_(protocol), channels_(std::move(channels)), policy_(policy)
{
}

LoggingProtocolId LoggingSession::protocol() const
{
    return protocol_;
}

const std::vector<LoggingChannel>& LoggingSession::channels() const
{
    return channels_;
}

const LoggingPolicy& LoggingSession::policy() const
{
    return policy_;
}

const LoggingChannel *LoggingSession::find_channel(std::string_view id) const
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

fastecu::Result<LoggingSession> make_logging_session(
    LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy)
{
    if (!valid_protocol(protocol) || policy.poll_timeout_ms <= 0 ||
        policy.car_silence_miss_threshold <= 0 ||
        policy.reconnect_attempt_threshold <= 0 || policy.reconnect_retry_period < 0)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "invalid logging policy");
    }
    if (protocol == LoggingProtocolId::Cdbg && channels.empty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "no CDBG log parameters selected");
    }

    std::unordered_set<std::string> ids;
    for (const LoggingChannel& channel : channels)
    {
        if (channel.id.empty() || !ids.insert(channel.id).second || channel.length == 0 ||
            channel.length > 255 || !valid_address(protocol, channel.address) ||
            !valid_raw_assembly(channel.raw_assembly) || channel.decimal_precision > 15 ||
            !valid_expression(channel))
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "invalid logging channel");
        }
    }

    return LoggingSession(protocol, std::move(channels), policy);
}
