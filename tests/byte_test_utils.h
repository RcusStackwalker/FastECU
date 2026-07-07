#pragma once

#include "protocol/bytes.h"

#include <string_view>

namespace test_bytes
{

inline int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

inline bytes::Bytes bytesFromHex(std::string_view hex)
{
    bytes::Bytes out;
    int high = -1;
    for (char c : hex)
    {
        const int nibble = hexNibble(c);
        if (nibble < 0)
            continue;
        if (high < 0)
        {
            high = nibble;
            continue;
        }
        out.push_back(static_cast<bytes::Byte>((high << 4) | nibble));
        high = -1;
    }
    return out;
}

} // namespace test_bytes
