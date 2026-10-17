#pragma once

#include <gmock/gmock.h>

#include <cstddef>

#include "src/algorithms/protocol/bytes.h"

namespace test_bytes
{

// Full ROMs contain millions of bytes. Avoid constructing a polymorphic
// matcher/listener for each byte, and keep failures bounded to one offset.
MATCHER_P(BytesFilledWith, value, "")
{
    std::size_t offset = 0;
    for (const auto byte : arg)
    {
        if (byte != value)
        {
            *result_listener << "byte #" << offset << " is " << bytes::ToHex(bytes::ByteView(&byte, 1));
            return false;
        }
        ++offset;
    }
    return true;
}

// Compares any byte range against any byte range and explains a mismatch as
// hex. A matcher rather than a PrintTo because bytes::Bytes and
// bytes::ByteView are std::vector/std::span: their only associated namespace
// is std, so ADL would never find a PrintTo declared in namespace bytes.
MATCHER_P(BytesEq, expected, "")
{
    const bytes::Bytes actual_bytes(arg.begin(), arg.end());
    const bytes::Bytes expected_bytes(expected.begin(), expected.end());
    if (actual_bytes == expected_bytes)
    {
        return true;
    }
    *result_listener << "actual   " << bytes::ToHex(actual_bytes) << "\n"
                     << "expected " << bytes::ToHex(expected_bytes);
    return false;
}

} // namespace test_bytes
