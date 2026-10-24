#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <stdexcept>
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

namespace literals
{

// 0x34_b -> Byte. consteval, so an out-of-range literal is a compile error
// rather than a silent truncation: a throw-expression is not a constant
// expression, so the call cannot be evaluated.
consteval Byte operator""_b(unsigned long long value)
{
    return value <= 0xFF ? static_cast<Byte>(value) : throw std::out_of_range("byte literal does not fit in one byte");
}

} // namespace literals

template <typename T>
concept ByteRange = std::ranges::input_range<T> && std::same_as<std::ranges::range_value_t<T>, Byte>;

// A sized range of unsigned integers wider than a byte, each composed
// big-endian at its own width. Sized so the output is allocated once and
// filled in place; ROM-sized payloads go through here.
template <typename T>
concept WideIntegerRange =
    std::ranges::input_range<T> && std::ranges::sized_range<T> &&
    std::unsigned_integral<std::ranges::range_value_t<T>> && (sizeof(std::ranges::range_value_t<T>) > 1);

namespace detail
{

// Deliberately not `static_assert(false, ...)`: P2593R1 support in MSVC is
// newer than the rest of what this repo relies on, and MSVC builds with
// /std:c++latest.
template <typename> inline constexpr bool kDependentFalse = false;

template <typename T> constexpr std::size_t WidthBe(const T& arg)
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
    else if constexpr (WideIntegerRange<U>)
    {
        return sizeof(std::ranges::range_value_t<U>) * std::ranges::size(arg);
    }
    else
    {
        static_assert(
            kDependentFalse<U>,
            "composeBe: argument must be Byte, std::uint16_t, u24(), "
            "std::uint32_t, std::string_view, or a range of Byte or "
            "a range of std::uint16_t, std::uint32_t or std::uint64_t. A bare integer literal is an int -- write "
            "0x34_b instead.");
        return 0;
    }
}

// Only these range types carry a length in their type. Other sized ranges
// still have a runtime length, even when their storage happens to be fixed.
template <typename T>
inline constexpr std::size_t kStaticExtent = std::is_bounded_array_v<T> ? std::extent_v<T> : std::dynamic_extent;
template <typename T, std::size_t N> inline constexpr std::size_t kStaticExtent<std::array<T, N>> = N;
template <typename T, std::size_t N> inline constexpr std::size_t kStaticExtent<std::span<T, N>> = N;

template <typename T> consteval std::size_t StaticWidthBe()
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, Byte> || std::same_as<U, std::uint16_t> || std::same_as<U, std::uint32_t>)
    {
        return sizeof(U);
    }
    else if constexpr (std::same_as<U, U24>)
    {
        return 3;
    }
    else if constexpr ((ByteRange<U> || WideIntegerRange<U>) && kStaticExtent<U> != std::dynamic_extent)
    {
        return kStaticExtent<U> * sizeof(std::ranges::range_value_t<U>);
    }
    else
    {
        return std::dynamic_extent;
    }
}

template <typename... Args>
inline constexpr bool kStaticComposition = ((StaticWidthBe<Args>() != std::dynamic_extent) && ...);

template <std::size_t Width, typename Output, typename Integer> constexpr void EmitIntegerBe(Output& out, Integer value)
{
    for (std::size_t shift = Width; shift-- > 0;)
    {
        *out++ = static_cast<Byte>(value >> (8 * shift));
    }
}

// Output is either an array iterator or a vector back inserter: the same
// encoding law applies without an intermediate container on the static path.
template <typename Output, typename T> constexpr void EmitBe(Output& out, const T& arg)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, Byte> || std::same_as<U, std::uint16_t> || std::same_as<U, std::uint32_t>)
    {
        EmitIntegerBe<sizeof(U)>(out, arg);
    }
    else if constexpr (std::same_as<U, U24>)
    {
        EmitIntegerBe<3>(out, arg.value);
    }
    else if constexpr (std::same_as<U, std::string_view> || ByteRange<U>)
    {
        for (const auto value : arg)
        {
            *out++ = static_cast<Byte>(value);
        }
    }
    else if constexpr (WideIntegerRange<U>)
    {
        using Value = std::ranges::range_value_t<U>;
        for (const auto value : arg)
        {
            EmitIntegerBe<sizeof(Value)>(out, static_cast<Value>(value));
        }
    }
    else
    {
        static_assert(kDependentFalse<U>, "ComposeBe: unsupported field type; use Byte, std::uint16_t, U24, "
                                          "std::uint32_t, std::string_view, or a supported unsigned integer range. "
                                          "A bare integer literal is an int -- write 0x34_b instead.");
    }
}

// Preserve bulk insertion and in-place wide-range encoding for dynamic
// payloads; large ROM ranges should not pay a push_back check per byte.
template <typename T> void AppendBe(Bytes& out, const T& arg)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (ByteRange<U>)
    {
        out.insert(out.end(), std::ranges::begin(arg), std::ranges::end(arg));
    }
    else if constexpr (WideIntegerRange<U>)
    {
        const auto offset = out.size();
        out.resize(offset + WidthBe(arg));
        auto next = out.begin() + static_cast<Bytes::difference_type>(offset);
        EmitBe(next, arg);
    }
    else
    {
        auto next = std::back_inserter(out);
        EmitBe(next, arg);
    }
}

} // namespace detail

// Composes `args` big-endian, reserving `extra_capacity` bytes beyond the
// composed length so a caller that appends afterwards does not reallocate.
template <typename... Args> Bytes ComposeBeWithExtraCapacity(std::size_t extra_capacity, const Args&...args)
{
    Bytes out;
    out.reserve(extra_capacity + (std::size_t{0} + ... + detail::WidthBe(args)));
    (detail::AppendBe(out, args), ...);
    return out;
}

// Type-known lengths produce an allocation-free array; runtime lengths
// produce Bytes. A statically empty composition is a caller error.
template <typename... Args> constexpr auto ComposeBe(const Args&...args)
{
    if constexpr (detail::kStaticComposition<Args...>)
    {
        constexpr auto kSize = (std::size_t{0} + ... + detail::StaticWidthBe<Args>());
        static_assert(kSize > 0, "ComposeBe: statically empty output is not supported");
        std::array<Byte, kSize> out{};
        auto next = out.begin();
        (detail::EmitBe(next, args), ...);
        return out;
    }
    else
    {
        return ComposeBeWithExtraCapacity(0, args...);
    }
}

// Composes `args` big-endian, then appends `checksum(composed)` using the
// same width law -- a Byte-returning function appends one byte, a
// uint32_t-returning one appends four, most-significant first.
template <typename ChecksumFn, typename... Args>
constexpr auto ComposeBeWithChecksum(ChecksumFn checksum, const Args&...args)
{
    using Sum = std::invoke_result_t<ChecksumFn, ByteView>;
    static_assert(std::unsigned_integral<Sum> && sizeof(Sum) <= 4,
                  "composeBeWithChecksum: checksum function must return Byte, "
                  "std::uint16_t, or std::uint32_t -- appendBe has no wider width to give it");
    if constexpr (detail::kStaticComposition<Args...>)
    {
        constexpr auto kPayloadSize = (std::size_t{0} + ... + detail::StaticWidthBe<Args>());
        std::array<Byte, kPayloadSize + sizeof(Sum)> out{};
        auto next = out.begin();
        (detail::EmitBe(next, args), ...);
        detail::EmitBe(next, static_cast<Sum>(checksum(ByteView(out).first(kPayloadSize))));
        return out;
    }
    else
    {
        Bytes out = ComposeBeWithExtraCapacity(sizeof(Sum), args...);
        detail::AppendBe(out, static_cast<Sum>(checksum(ByteView(out))));
        return out;
    }
}

} // namespace bytes
