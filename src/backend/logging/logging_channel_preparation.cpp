#include "src/backend/logging/logging_channel_preparation.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

#include "src/backend/logging/logging_session.h"

namespace fastecu::logging
{
namespace
{
std::string_view TrimAscii(std::string_view input)
{
    constexpr std::string_view kWhitespace = " \t\n\r\f\v";
    const auto first = input.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return input.substr(first, input.find_last_not_of(kWhitespace) - first + 1);
}

template <typename T> bool ParseUnsigned(std::string_view input, int base, T& value)
{
    input = TrimAscii(input);
    if (base == 16 && (input.starts_with("0x") || input.starts_with("0X")))
    {
        input.remove_prefix(2);
    }
    if (input.empty())
    {
        return false;
    }
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value, base);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

fastecu::Error ContextualError(const LoggerParameter& parameter, std::string_view detail)
{
    return {ErrorKind::kInvalidConfig, std::format("{} parameter {}: {}", parameter.protocol, parameter.id, detail)};
}
} // namespace

fastecu::Result<LoggingChannel> PrepareLoggingChannel(const LoggerParameter& parameter, LoggingProtocolId protocol)
{
    if (parameter.conversions.empty())
    {
        return std::unexpected(ContextualError(parameter, "missing conversion; provide an expression and format"));
    }
    const auto& conversion = parameter.conversions.front();
    std::uint32_t address = 0;
    if (!ParseUnsigned(parameter.address, 16, address))
    {
        return std::unexpected(ContextualError(
            parameter, std::format("invalid address '{}'; expected hexadecimal digits with an optional 0x prefix",
                                   parameter.address)));
    }
    std::size_t length = 0;
    if (!ParseUnsigned(parameter.length, 10, length) || length == 0)
    {
        return std::unexpected(ContextualError(
            parameter, std::format("invalid length '{}'; expected a positive decimal integer", parameter.length)));
    }
    if ((protocol == LoggingProtocolId::kMutDma || protocol == LoggingProtocolId::kCdbg) && length != 1 &&
        length != 2 && length != 4)
    {
        return std::unexpected(
            ContextualError(parameter, "invalid length; expected 1, 2 or 4 bytes for this protocol"));
    }
    const std::string_view format = conversion.format;
    std::size_t precision = 0;
    if (format != "0")
    {
        if (!format.starts_with("0.") || format.size() < 3 || format.size() > 17 ||
            !std::ranges::all_of(format.substr(2), [](char ch) { return ch == '0'; }))
        {
            return std::unexpected(ContextualError(
                parameter, std::format("invalid format '{}'; expected 0 or 0. followed by 1–15 zeros", format)));
        }
        precision = format.size() - 2;
    }
    LoggingChannel channel{
        .id = parameter.id,
        .address = address,
        .length = length,
        .raw_assembly = protocol == LoggingProtocolId::kSsm ? RawAssembly::kDecimalBytesConcatenated
                                                            : RawAssembly::kUnsignedIntegerDecimal,
        .from_byte_expression = conversion.expr,
        .unit = conversion.units,
        .decimal_precision = static_cast<std::uint8_t>(precision),
    };
    if (const auto valid = ValidateLoggingChannel(protocol, channel); !valid.has_value())
    {
        return std::unexpected(ContextualError(parameter, valid.error().detail));
    }
    return channel;
}
} // namespace fastecu::logging
