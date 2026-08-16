#pragma once

#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace fastecu::definition
{

inline std::string hex_text(std::uint64_t value)
{
    return std::format("0x{:x}", value);
}

// The pure hex parse shared by parser_utils.cpp's parse_hex_unsigned (which
// wraps it in definition-parser error context) and the flash EEPROM read
// plan use case (which needs it without pugixml or that context). Extracted
// rather than reimplemented -- a second std::from_chars wrapper is exactly
// what issue #120 was filed about.
//
// Trims surrounding whitespace (the same set std::isspace matches in the C
// locale -- ' ', '\t', '\r', '\n', '\v', '\f' -- matching parser_utils.cpp's
// trim_copy exactly, since this is a behavior-preserving extraction of its
// hex-parsing core), accepts an optional "0x"/"0X" prefix, and requires the
// entire remainder to be consumed, so trailing junk is a rejection rather
// than a partial parse. Matches Qt's QString::toUInt(&ok, 16) on every value
// in the shipped protocols.cfg plus \v/\f-padded inputs, pinned by
// tests/test_hex_parse_qt_compat.cpp.
inline std::optional<std::uint64_t> parse_hex_value(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto last = text.find_last_not_of(" \t\r\n\v\f");
    std::string_view digits = text.substr(first, last - first + 1);

    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }
    // Not load-bearing for correctness -- std::from_chars on an empty range
    // already returns errc::invalid_argument, which the check below rejects
    // -- but it documents the "0x" case explicitly rather than relying on
    // that from_chars behavior implicitly.
    if (digits.empty())
    {
        return std::nullopt;
    }

    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, 16);
    if (error != std::errc{} || end != digits.data() + digits.size())
    {
        return std::nullopt;
    }
    return parsed;
}

} // namespace fastecu::definition
