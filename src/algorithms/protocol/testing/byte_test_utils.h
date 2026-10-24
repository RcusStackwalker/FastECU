#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <initializer_list>
#include <ranges>
#include <string_view>

namespace test_bytes
{

inline int HexNibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

inline bytes::Bytes BytesFromHex(std::string_view hex)
{
    bytes::Bytes out;
    int high = -1;
    for (char c : hex)
    {
        const int nibble = HexNibble(c);
        if (nibble < 0)
        {
            continue;
        }
        if (high < 0)
        {
            high = nibble;
            continue;
        }
        out.push_back(static_cast<bytes::Byte>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(nibble)));
        high = -1;
    }
    return out;
}

// The bytes of `text`, for feeding XML or other text to a byte-based port.
inline bytes::Bytes BytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
}

// A byte vector from int literals, so tests can write B({0x01, 0xFF}).
inline bytes::Bytes B(std::initializer_list<int> values)
{
    return values | std::views::transform([](int v) { return static_cast<bytes::Byte>(v); }) |
           std::ranges::to<bytes::Bytes>();
}

} // namespace test_bytes
