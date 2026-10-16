#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
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

// The loops behind FoldU32Be and SumU16Be, one piece at a time.
void FoldPieceU32Be(WordFold& fold, bytes::ByteView piece);
void SumPieceU16Be(std::uint16_t& sum, bytes::ByteView piece);

// The pieces of a checksum window, each a view of the ROM; a checksum that
// leaves out a slot folds the pieces either side of it.
//
// Every piece must be a whole number of words (halfwords for SumU16Be). A
// fixed-size piece, such as image.subspan<0x6000, 0x2000>() on a fixed-size
// ROM, is checked at compile time. A runtime piece is the caller's to check:
// its window comes from a constexpr table the caller static_asserts, or from
// a ROM size the caller validated on entry.
template <typename R>
concept ByteViewRange =
    std::ranges::input_range<R> && std::convertible_to<std::ranges::range_reference_t<R>, bytes::ByteView>;

// The 32-bit sum and XOR of the big-endian words of every piece.
template <std::size_t... Extents>
    requires((Extents == std::dynamic_extent || Extents % 4 == 0) && ...)
WordFold FoldU32Be(std::span<const bytes::Byte, Extents>... pieces)
{
    WordFold fold;
    (FoldPieceU32Be(fold, pieces), ...);
    return fold;
}

// The 16-bit sum of the big-endian halfwords of every piece.
template <std::size_t... Extents>
    requires((Extents == std::dynamic_extent || Extents % 2 == 0) && ...)
std::uint16_t SumU16Be(std::span<const bytes::Byte, Extents>... pieces)
{
    std::uint16_t sum = 0;
    (SumPieceU16Be(sum, pieces), ...);
    return sum;
}

template <ByteViewRange Pieces> std::uint16_t SumU16Be(Pieces&& pieces)
{
    std::uint16_t sum = 0;
    for (const bytes::ByteView piece : pieces)
    {
        SumPieceU16Be(sum, piece);
    }
    return sum;
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
