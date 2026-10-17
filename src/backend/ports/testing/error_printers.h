#pragma once

#include <expected>
#include <ostream>
#include <type_traits>

#include <gtest/gtest-printers.h>

#include "src/backend/ports/error.h"

namespace fastecu
{

// GoogleTest finds these by argument-dependent lookup: both types live in
// namespace fastecu. Without them, every EXPECT_EQ on an ErrorKind fails with
// "4-byte object <01-00 00-00>" on both sides.
inline void PrintTo(ErrorKind kind, std::ostream *os)
{
    *os << ToString(kind);
}

inline void PrintTo(const Error& error, std::ostream *os)
{
    *os << ToString(error.kind);
    if (!error.detail.empty())
    {
        *os << " (" << error.detail << ")";
    }
}

// Result<T> and Status: fastecu::Error is a template argument of the
// std::expected, which makes fastecu an associated namespace for ADL.
template <typename T> void PrintTo(const std::expected<T, Error>& result, std::ostream *os)
{
    if (!result.has_value())
    {
        *os << "error: ";
        PrintTo(result.error(), os);
        return;
    }
    if constexpr (std::is_void_v<T>)
    {
        *os << "ok";
    }
    else
    {
        *os << "ok: " << ::testing::PrintToString(*result);
    }
}

} // namespace fastecu
