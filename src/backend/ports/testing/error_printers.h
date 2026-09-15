#pragma once

#include <ostream>

#include "src/backend/ports/error.h"

namespace fastecu
{

// GoogleTest finds these by argument-dependent lookup: both types live in
// namespace fastecu. Without them, every EXPECT_EQ on an ErrorKind fails with
// "4-byte object <01-00 00-00>" on both sides.
inline void PrintTo(ErrorKind kind, std::ostream *os)
{
    *os << to_string(kind);
}

inline void PrintTo(const Error& error, std::ostream *os)
{
    *os << to_string(error.kind);
    if (!error.detail.empty())
    {
        *os << " (" << error.detail << ")";
    }
}

} // namespace fastecu
