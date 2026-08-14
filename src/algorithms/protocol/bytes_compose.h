#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace bytes
{

// 24-bit field marker. uint32_t alone cannot say 3 bytes vs 4, and both
// widths occur in the ECU frame formats this composes.
struct U24
{
    std::uint32_t value;
};

constexpr U24 u24(std::uint32_t value) noexcept
{
    return U24{value};
}

namespace literals
{

// 0x34_b -> Byte. consteval, so an out-of-range literal is a compile error
// rather than a silent truncation: a throw-expression is not a constant
// expression, so the call cannot be evaluated.
consteval Byte operator""_b(unsigned long long value)
{
    return value <= 0xFF ? static_cast<Byte>(value)
                         : throw "byte literal does not fit in one byte";
}

} // namespace literals

template <typename T>
concept ByteRange =
    std::ranges::input_range<T> && std::same_as<std::ranges::range_value_t<T>, Byte>;

namespace detail
{

// Deliberately not `static_assert(false, ...)`: P2593R1 support in MSVC is
// newer than the rest of what this repo relies on, and MSVC builds with
// /std:c++latest.
template <typename>
inline constexpr bool dependentFalse = false;

template <typename T>
constexpr std::size_t widthBe(const T& arg)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, Byte>)
    {
        return 1;
    }
    else if constexpr (std::same_as<U, std::uint16_t>)
    {
        return 2;
    }
    else if constexpr (std::same_as<U, U24>)
    {
        return 3;
    }
    else if constexpr (std::same_as<U, std::uint32_t>)
    {
        return 4;
    }
    else if constexpr (std::same_as<U, std::string_view>)
    {
        return arg.size();
    }
    else if constexpr (ByteRange<U>)
    {
        if constexpr (std::ranges::sized_range<U>)
        {
            return std::ranges::size(arg);
        }
        else
        {
            return 0; // capacity is a hint, never correctness
        }
    }
    else
    {
        static_assert(dependentFalse<U>,
                      "composeBe: argument must be Byte, std::uint16_t, u24(), "
                      "std::uint32_t, std::string_view, or a range of Byte. A bare "
                      "integer literal is an int -- write 0x34_b instead.");
        return 0;
    }
}

template <typename T>
void appendBe(Bytes& out, const T& arg)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, Byte>)
    {
        out.push_back(arg);
    }
    else if constexpr (std::same_as<U, std::uint16_t>)
    {
        appendU16Be(out, arg);
    }
    else if constexpr (std::same_as<U, U24>)
    {
        appendU24Be(out, arg.value);
    }
    else if constexpr (std::same_as<U, std::uint32_t>)
    {
        appendU32Be(out, arg);
    }
    else if constexpr (std::same_as<U, std::string_view>)
    {
        for (const char character : arg)
        {
            out.push_back(static_cast<Byte>(character));
        }
    }
    else if constexpr (ByteRange<U>)
    {
        out.insert(out.end(), std::ranges::begin(arg), std::ranges::end(arg));
    }
    else
    {
        static_assert(dependentFalse<U>,
                      "composeBe: argument must be Byte, std::uint16_t, u24(), "
                      "std::uint32_t, std::string_view, or a range of Byte. A bare "
                      "integer literal is an int -- write 0x34_b instead.");
    }
}

} // namespace detail

// Composes `args` big-endian, reserving `extra_capacity` bytes beyond the
// composed length so a caller that appends afterwards does not reallocate.
template <typename... Args>
Bytes composeBeWithExtraCapacity(std::size_t extra_capacity, const Args&...args)
{
    Bytes out;
    out.reserve(extra_capacity + (std::size_t{0} + ... + detail::widthBe(args)));
    (detail::appendBe(out, args), ...);
    return out;
}

template <typename... Args>
Bytes composeBe(const Args&...args)
{
    return composeBeWithExtraCapacity(0, args...);
}

// Composes `args` big-endian, then appends `checksum(composed)` using the
// same width law -- a Byte-returning function appends one byte, a
// uint32_t-returning one appends four, most-significant first.
template <typename ChecksumFn, typename... Args>
Bytes composeBeWithChecksum(ChecksumFn checksum, const Args&...args)
{
    using Sum = std::invoke_result_t<ChecksumFn, ByteView>;
    static_assert(std::unsigned_integral<Sum> && sizeof(Sum) <= 4,
                  "composeBeWithChecksum: checksum function must return Byte, "
                  "std::uint16_t, or std::uint32_t -- appendBe has no wider width to give it");
    Bytes out = composeBeWithExtraCapacity(sizeof(Sum), args...);
    detail::appendBe(out, static_cast<Sum>(checksum(ByteView(out))));
    return out;
}

} // namespace bytes
