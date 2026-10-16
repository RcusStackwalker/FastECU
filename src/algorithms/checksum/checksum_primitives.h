#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>

namespace fastecu::checksum::internal
{

void RebalanceU16Be(bytes::MutableByteView rom, std::size_t offset, std::uint16_t observed, std::uint16_t target);
void RebalanceU32Be(bytes::MutableByteView rom, std::size_t offset, std::uint32_t observed, std::uint32_t target);

struct WordFold
{
    std::uint32_t sum = 0;
    std::uint32_t xor_all = 0;
};

// The pieces of a checksum window, each a view of the ROM; a checksum that
// leaves out a slot folds the pieces either side of it.
template <typename R>
concept ByteViewRange =
    std::ranges::input_range<R> && std::convertible_to<std::ranges::range_reference_t<R>, bytes::ByteView>;

// The 32-bit sum and XOR of the big-endian words of every piece. A piece's
// trailing partial word is left out.
template <ByteViewRange Pieces> WordFold FoldU32Be(Pieces&& pieces)
{
    WordFold fold;
    for (const bytes::ByteView piece : pieces)
    {
        for (std::size_t offset = 0; offset + 4 <= piece.size(); offset += 4)
        {
            const std::uint32_t word = bytes::ReadU32Be(piece, offset);
            fold.sum += word;
            fold.xor_all ^= word;
        }
    }
    return fold;
}

inline WordFold FoldU32Be(std::initializer_list<bytes::ByteView> pieces)
{
    return FoldU32Be(std::span(pieces));
}

// The 16-bit sum of the big-endian halfwords of every piece. A piece's
// trailing odd byte is left out.
template <ByteViewRange Pieces> std::uint16_t SumU16Be(Pieces&& pieces)
{
    std::uint16_t sum = 0;
    for (const bytes::ByteView piece : pieces)
    {
        for (std::size_t offset = 0; offset + 2 <= piece.size(); offset += 2)
        {
            sum = static_cast<std::uint16_t>(sum + bytes::ReadU16Be(piece, offset));
        }
    }
    return sum;
}

inline std::uint16_t SumU16Be(std::initializer_list<bytes::ByteView> pieces)
{
    return SumU16Be(std::span(pieces));
}

} // namespace fastecu::checksum::internal

namespace fastecu::checksum
{

// One's-complement-style 8-bit checksum: sum bytes, and whenever the
// running sum overflows 8 bits, add 1 back in before truncating (rather
// than a plain mod-256 sum). Used to checksum ECU reflash blocks.
std::uint8_t CksAdd8(std::span<const std::uint8_t> data);

// Additive 8-bit checksum complemented as 0x100 - sum, the framing
// convention used by the Subaru/Denso kernel-upload envelopes. The plain
// (uncomplemented) sum is bytes::sum8.
std::uint8_t NegatedSum8(bytes::ByteView data);

// CRC-32 with the ECU-reflash-specific polynomial 0x5AA5A55A (not the
// standard IEEE 802.3 polynomial). Used to verify a written flash page
// against the CRC the ECU reports back.
std::uint32_t Crc32(bytes::ByteView data);
std::uint32_t Crc32(const unsigned char *buf, std::uint32_t len);

} // namespace fastecu::checksum
