#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace bytes::views
{

namespace detail
{

struct U32BeFn : std::ranges::range_adaptor_closure<U32BeFn>
{
    // Each four bytes of `data` as one big-endian word; a trailing partial word
    // is ignored. The range is sized, random-access and in bounds by
    // construction, so reading a word needs no offset check.
    auto operator()(ByteView data) const
    {
        const auto read_word = [data](std::size_t word)
        {
            const std::span<const Byte, 4> b = data.subspan(word * 4).first<4>();
            return (std::uint32_t{b[0]} << 24U) | (std::uint32_t{b[1]} << 16U) | (std::uint32_t{b[2]} << 8U) |
                   std::uint32_t{b[3]};
        };
        return std::views::iota(std::size_t{0}, data.size() / 4) | std::views::transform(read_word);
    }
};

} // namespace detail

inline constexpr detail::U32BeFn kU32Be{};

} // namespace bytes::views
