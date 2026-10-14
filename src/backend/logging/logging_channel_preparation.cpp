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

std::string LowerAscii(std::string value)
{
    for (char& ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

Result<NumericStorage> ParseStorage(std::string_view input)
{
    const auto type = LowerAscii(std::string(input));
    if (type.empty() || type == "uint" || type == "uint8" || type == "uint16" || type == "uint32")
    {
        return NumericStorage::kUnsignedInteger;
    }
    if (type == "int" || type == "int8" || type == "int16" || type == "int32")
    {
        return NumericStorage::kSignedInteger;
    }
    if (type == "float")
    {
        return NumericStorage::kFloat;
    }
    return Fail(ErrorKind::kInvalidConfig, std::format("invalid storagetype '{}'", input));
}

Result<NumericByteOrder> ParseByteOrder(std::string_view input)
{
    const auto endian = LowerAscii(std::string(input));
    if (endian.empty() || endian == "big")
    {
        return NumericByteOrder::kBigEndian;
    }
    if (endian == "little")
    {
        return NumericByteOrder::kLittleEndian;
    }
    return Fail(ErrorKind::kInvalidConfig, std::format("invalid endian '{}'", input));
}

fastecu::Error ContextualError(const LoggerParameter& parameter, std::string_view detail)
{
    return {ErrorKind::kInvalidConfig, std::format("{} parameter {}: {}", parameter.protocol, parameter.id, detail)};
}
} // namespace

fastecu::Result<LoggingChannel> PrepareLoggingChannel(const LoggerParameter& parameter, LoggingProtocolId protocol)
{
    // #lizard forgives(cyclomatic_complexity)
    // TODO: Reduce complexity and remove forgiveness; see docs/tech-debt.md.
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
    std::vector<std::uint32_t> byte_addresses;
    if (!parameter.address_specs.empty())
    {
        const bool explicit_bytes = parameter.address_specs.size() > 1;
        if (explicit_bytes && protocol != LoggingProtocolId::kSsm)
        {
            return std::unexpected(ContextualError(parameter, "explicit byte address lists require SSM"));
        }
        for (const auto& source : parameter.address_specs)
        {
            std::uint32_t source_address = 0;
            if (!ParseUnsigned(source.value, 16, source_address))
            {
                return std::unexpected(ContextualError(parameter, "invalid explicit byte address"));
            }
            if (source.bit.has_value())
            {
                return std::unexpected(ContextualError(parameter, "sample bit is only valid for a switch"));
            }
            if (source.length.has_value())
            {
                std::size_t source_length = 0;
                if (!ParseUnsigned(*source.length, 10, source_length) || source_length == 0 ||
                    (explicit_bytes && source_length != 1))
                {
                    return std::unexpected(ContextualError(parameter, "invalid explicit address length"));
                }
                if (!explicit_bytes)
                {
                    length = source_length;
                }
            }
            byte_addresses.push_back(source_address);
        }
        if (explicit_bytes)
        {
            length = byte_addresses.size();
        }
        address = byte_addresses.front();
        if (!explicit_bytes)
        {
            byte_addresses.clear();
        } // A single base expands in the read planner.
    }
    if (length != 1 && length != 2 && length != 4)
    {
        return std::unexpected(ContextualError(parameter, "invalid length; expected 1, 2 or 4 bytes"));
    }
    const auto storage = ParseStorage(conversion.storage_type);
    if (!storage.has_value())
    {
        return std::unexpected(ContextualError(parameter, storage.error().detail));
    }
    const auto byte_order = ParseByteOrder(conversion.endian);
    if (!byte_order.has_value())
    {
        return std::unexpected(ContextualError(parameter, byte_order.error().detail));
    }
    if (*storage == NumericStorage::kFloat && length != 4)
    {
        return std::unexpected(ContextualError(parameter, "storagetype float requires length 4"));
    }
    if (protocol != LoggingProtocolId::kSsm && *storage != NumericStorage::kUnsignedInteger)
    {
        return std::unexpected(ContextualError(parameter, "storagetype is unsupported by this protocol"));
    }
    if (protocol != LoggingProtocolId::kSsm && *byte_order != NumericByteOrder::kBigEndian)
    {
        return std::unexpected(ContextualError(parameter, "endian override is unsupported by this protocol"));
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
        .from_byte_expression = conversion.expr,
        .unit = conversion.units,
        .decimal_precision = static_cast<std::uint8_t>(precision),
        .byte_addresses = std::move(byte_addresses),
        .storage = *storage,
        .byte_order = *byte_order,
    };
    if (const auto valid = ValidateLoggingChannel(protocol, channel); !valid.has_value())
    {
        return std::unexpected(ContextualError(parameter, valid.error().detail));
    }
    return channel;
}
Result<LoggingChannel> PrepareLoggingSwitch(const LoggerSwitch& source, LoggingProtocolId protocol)
{
    std::uint8_t bit = 0;
    if (!ParseUnsigned(source.sample_bit, 10, bit) || bit >= 8)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} switch {}: invalid sample bit; expected 0–7", source.protocol, source.id));
    }
    LoggerParameter parameter{.protocol = source.protocol,
                              .id = source.id,
                              .name = source.name,
                              .address = source.address,
                              .length = "1",
                              .conversions = {{.units = "", .expr = "x", .format = "0"}}};
    auto result = PrepareLoggingChannel(parameter, protocol);
    if (!result.has_value())
    {
        return std::unexpected(result.error());
    }
    result->sample_bit = bit;
    if (const auto valid = ValidateLoggingChannel(protocol, *result); !valid.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} switch {}: {}", source.protocol, source.id, valid.error().detail));
    }
    return result;
}
} // namespace fastecu::logging
