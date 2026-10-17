#include "src/backend/logging/logging_channel_preparation.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Parsers below report a bare detail; PrepareLoggingChannel adds the
// parameter's protocol and id.
template <typename T> using Parsed = std::expected<T, std::string>;

Parsed<NumericStorage> ParseStorage(std::string_view input)
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
    return std::unexpected(std::format("invalid storagetype '{}'", input));
}

Parsed<NumericByteOrder> ParseByteOrder(std::string_view input)
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
    return std::unexpected(std::format("invalid endian '{}'", input));
}

struct Placement
{
    std::uint32_t address = 0;
    std::size_t length = 0;
    // Empty unless the definition lists each SSM byte explicitly.
    std::vector<std::uint32_t> byte_addresses;
};

// Returns the spec's address; a spec with a length also sets the placement's.
// Explicitly listed bytes are one byte each.
Parsed<std::uint32_t> ParseAddressSpec(const LoggerAddressSpec& source, bool explicit_bytes, Placement& placement)
{
    std::uint32_t address = 0;
    if (!ParseUnsigned(source.value, 16, address))
    {
        return std::unexpected("invalid explicit byte address");
    }
    if (source.bit.has_value())
    {
        return std::unexpected("sample bit is only valid for a switch");
    }
    if (source.length.has_value() && (!ParseUnsigned(*source.length, 10, placement.length) || placement.length == 0 ||
                                      (explicit_bytes && placement.length != 1)))
    {
        return std::unexpected("invalid explicit address length");
    }
    return address;
}

// One address spec relocates the parameter; a single base expands in the
// read planner. Several specs list SSM bytes explicitly.
Parsed<void> ApplyAddressSpecs(const LoggerParameter& parameter, LoggingProtocolId protocol, Placement& placement)
{
    const bool explicit_bytes = parameter.address_specs.size() > 1;
    if (explicit_bytes && protocol != LoggingProtocolId::kSsm)
    {
        return std::unexpected("explicit byte address lists require SSM");
    }
    std::vector<std::uint32_t> addresses;
    for (const auto& source : parameter.address_specs)
    {
        auto address = ParseAddressSpec(source, explicit_bytes, placement);
        if (!address.has_value())
        {
            return std::unexpected(std::move(address.error()));
        }
        addresses.push_back(*address);
    }
    if (!addresses.empty())
    {
        placement.address = addresses.front();
    }
    if (explicit_bytes)
    {
        placement.length = addresses.size();
        placement.byte_addresses = std::move(addresses);
    }
    return {};
}

Parsed<Placement> ParsePlacement(const LoggerParameter& parameter, LoggingProtocolId protocol)
{
    Placement placement;
    if (!ParseUnsigned(parameter.address, 16, placement.address))
    {
        return std::unexpected(std::format(
            "invalid address '{}'; expected hexadecimal digits with an optional 0x prefix", parameter.address));
    }
    if (!ParseUnsigned(parameter.length, 10, placement.length) || placement.length == 0)
    {
        return std::unexpected(
            std::format("invalid length '{}'; expected a positive decimal integer", parameter.length));
    }
    if (auto applied = ApplyAddressSpecs(parameter, protocol, placement); !applied.has_value())
    {
        return std::unexpected(std::move(applied.error()));
    }
    if (placement.length != 1 && placement.length != 2 && placement.length != 4)
    {
        return std::unexpected("invalid length; expected 1, 2 or 4 bytes");
    }
    return placement;
}

struct Encoding
{
    NumericStorage storage;
    NumericByteOrder byte_order;
};

// Only SSM decodes anything other than big-endian unsigned integers.
Parsed<Encoding> ParseEncoding(const Conversion& conversion, LoggingProtocolId protocol, std::size_t length)
{
    const auto storage = ParseStorage(conversion.storage_type);
    if (!storage.has_value())
    {
        return std::unexpected(storage.error());
    }
    const auto byte_order = ParseByteOrder(conversion.endian);
    if (!byte_order.has_value())
    {
        return std::unexpected(byte_order.error());
    }
    if (*storage == NumericStorage::kFloat && length != 4)
    {
        return std::unexpected("storagetype float requires length 4");
    }
    if (protocol != LoggingProtocolId::kSsm && *storage != NumericStorage::kUnsignedInteger)
    {
        return std::unexpected("storagetype is unsupported by this protocol");
    }
    if (protocol != LoggingProtocolId::kSsm && *byte_order != NumericByteOrder::kBigEndian)
    {
        return std::unexpected("endian override is unsupported by this protocol");
    }
    return Encoding{*storage, *byte_order};
}

// "0" or "0." followed by 1-15 zeros, giving that many decimal places.
Parsed<std::uint8_t> ParsePrecision(std::string_view format)
{
    if (format == "0")
    {
        return 0;
    }
    if (!format.starts_with("0.") || format.size() < 3 || format.size() > 17 ||
        !std::ranges::all_of(format.substr(2), [](char ch) { return ch == '0'; }))
    {
        return std::unexpected(std::format("invalid format '{}'; expected 0 or 0. followed by 1–15 zeros", format));
    }
    return static_cast<std::uint8_t>(format.size() - 2);
}

Parsed<LoggingChannel> ParseChannel(const LoggerParameter& parameter, LoggingProtocolId protocol)
{
    if (parameter.conversions.empty())
    {
        return std::unexpected("missing conversion; provide an expression and format");
    }
    const auto& conversion = parameter.conversions.front();
    auto placement = ParsePlacement(parameter, protocol);
    if (!placement.has_value())
    {
        return std::unexpected(std::move(placement.error()));
    }
    const auto encoding = ParseEncoding(conversion, protocol, placement->length);
    if (!encoding.has_value())
    {
        return std::unexpected(encoding.error());
    }
    const auto precision = ParsePrecision(conversion.format);
    if (!precision.has_value())
    {
        return std::unexpected(precision.error());
    }
    LoggingChannel channel{
        .id = parameter.id,
        .address = placement->address,
        .length = placement->length,
        .from_byte_expression = conversion.expr,
        .unit = conversion.units,
        .decimal_precision = *precision,
        .byte_addresses = std::move(placement->byte_addresses),
        .storage = encoding->storage,
        .byte_order = encoding->byte_order,
    };
    if (const auto valid = ValidateLoggingChannel(protocol, channel); !valid.has_value())
    {
        return std::unexpected(valid.error().detail);
    }
    return channel;
}
} // namespace

fastecu::Result<LoggingChannel> PrepareLoggingChannel(const LoggerParameter& parameter, LoggingProtocolId protocol)
{
    auto channel = ParseChannel(parameter, protocol);
    if (!channel.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} parameter {}: {}", parameter.protocol, parameter.id, channel.error()));
    }
    return *std::move(channel);
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
