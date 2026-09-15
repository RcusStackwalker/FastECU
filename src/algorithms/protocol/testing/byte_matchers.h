#pragma once

#include <gmock/gmock.h>

#include "src/algorithms/protocol/bytes.h"

namespace test_bytes
{

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
    *result_listener << "actual   " << bytes::toHex(actual_bytes) << "\n"
                     << "expected " << bytes::toHex(expected_bytes);
    return false;
}

} // namespace test_bytes
