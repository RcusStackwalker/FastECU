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
std::string_view trim_ascii(std::string_view input)
{
    constexpr std::string_view kWhitespace = " \t\n\r\f\v";
    const auto first = input.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return input.substr(first, input.find_last_not_of(kWhitespace) - first + 1);
}

template <typename T> bool parse_unsigned(std::string_view input, int base, T& value)
{
    input = trim_ascii(input);
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

fastecu::Error contextual_error(const LoggerParameter& parameter, std::string_view detail)
{
    return {ErrorKind::InvalidConfig, std::format("{} parameter {}: {}", parameter.protocol, parameter.id, detail)};
}
} // namespace

fastecu::Result<LoggingChannel> prepare_logging_channel(const LoggerParameter& parameter, LoggingProtocolId protocol)
{
    if (parameter.conversions.empty())
    {
        return std::unexpected(contextual_error(parameter, "missing conversion; provide an expression and format"));
    }
    const auto& conversion = parameter.conversions.front();
    std::uint32_t address = 0;
    if (!parse_unsigned(parameter.address, 16, address))
    {
        return std::unexpected(contextual_error(
            parameter, std::format("invalid address '{}'; expected hexadecimal digits with an optional 0x prefix",
                                   parameter.address)));
    }
    std::size_t length = 0;
    if (!parse_unsigned(parameter.length, 10, length) || length == 0)
    {
        return std::unexpected(contextual_error(
            parameter, std::format("invalid length '{}'; expected a positive decimal integer", parameter.length)));
    }
    std::vector<std::uint32_t> byte_addresses;
    if (!parameter.address_specs.empty())
    {
        const bool explicit_bytes = parameter.address_specs.size() > 1;
        if (explicit_bytes && protocol != LoggingProtocolId::Ssm)
        {
            return std::unexpected(contextual_error(parameter, "explicit byte address lists require SSM"));
        }
        for (const auto& source : parameter.address_specs)
        {
            std::uint32_t source_address = 0;
            if (!parse_unsigned(source.value, 16, source_address))
            {
                return std::unexpected(contextual_error(parameter, "invalid explicit byte address"));
            }
            if (source.bit.has_value())
            {
                return std::unexpected(contextual_error(parameter, "sample bit is only valid for a switch"));
            }
            if (source.length.has_value())
            {
                std::size_t source_length = 0;
                if (!parse_unsigned(*source.length, 10, source_length) || source_length == 0 ||
                    (explicit_bytes && source_length != 1))
                {
                    return std::unexpected(contextual_error(parameter, "invalid explicit address length"));
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
        if (parameter.declared_length.has_value())
        {
            std::size_t declared = 0;
            if (!parse_unsigned(*parameter.declared_length, 10, declared) || declared != length)
            {
                return std::unexpected(contextual_error(parameter, "conflicting address/parameter length metadata"));
            }
        }
        address = byte_addresses.front();
        if (!explicit_bytes)
        {
            byte_addresses.clear();
        } // A single base expands in the read planner.
    }
    const std::string_view format = conversion.format;
    std::size_t precision = 0;
    if (format != "0")
    {
        if (!format.starts_with("0.") || format.size() < 3 || format.size() > 17 ||
            !std::ranges::all_of(format.substr(2), [](char ch) { return ch == '0'; }))
        {
            return std::unexpected(contextual_error(
                parameter, std::format("invalid format '{}'; expected 0 or 0. followed by 1–15 zeros", format)));
        }
        precision = format.size() - 2;
    }
    LoggingChannel channel{
        .id = parameter.id,
        .address = address,
        .length = length,
        .raw_assembly = protocol == LoggingProtocolId::Ssm ? RawAssembly::DecimalBytesConcatenated
                                                           : RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = conversion.expr,
        .unit = conversion.units,
        .decimal_precision = static_cast<std::uint8_t>(precision),
        .byte_addresses = std::move(byte_addresses),
    };
    if (const auto valid = validate_logging_channel(protocol, channel); !valid.has_value())
    {
        return std::unexpected(contextual_error(parameter, valid.error().detail));
    }
    return channel;
}
Result<LoggingChannel> prepare_logging_switch(const LoggerSwitch& source, LoggingProtocolId protocol)
{
    std::uint8_t bit = 0;
    if (!parse_unsigned(source.sample_bit, 10, bit) || bit >= 8)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} switch {}: invalid sample bit; expected 0–7", source.protocol, source.id));
    }
    if (source.declared_sample_address.has_value())
    {
        std::uint32_t declared = 0;
        std::uint32_t actual = 0;
        if (!parse_unsigned(*source.declared_sample_address, 16, declared) ||
            !parse_unsigned(source.address, 16, actual) || declared != actual)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("{} switch {}: conflicting or invalid sample address metadata", source.protocol,
                                    source.id));
        }
    }
    LoggerParameter parameter{.protocol = source.protocol,
                              .id = source.id,
                              .name = source.name,
                              .address = source.address,
                              .length = "1",
                              .conversions = {{.units = "", .expr = "x", .format = "0"}}};
    for (const auto& address : source.address_specs)
    {
        if (address.bit.has_value() && *address.bit != source.sample_bit)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("{} switch {}: conflicting sample bits", source.protocol, source.id));
        }
        auto normalized = address;
        normalized.bit.reset();
        parameter.address_specs.push_back(std::move(normalized));
    }
    auto result = prepare_logging_channel(parameter, protocol);
    if (!result.has_value())
    {
        return std::unexpected(result.error());
    }
    result->sample_bit = bit;
    if (const auto valid = validate_logging_channel(protocol, *result); !valid.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} switch {}: {}", source.protocol, source.id, valid.error().detail));
    }
    return result;
}
} // namespace fastecu::logging
