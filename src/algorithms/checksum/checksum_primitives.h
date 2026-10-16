#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fastecu::checksum::internal
{

void RebalanceU16Be(bytes::MutableByteView rom, std::size_t offset, std::uint16_t observed, std::uint16_t target);
void RebalanceU32Be(bytes::MutableByteView rom, std::size_t offset, std::uint32_t observed, std::uint32_t target);

// The ROM addresses [begin, end).
struct AddressRange
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct WordFold
{
    std::uint32_t sum = 0;
    std::uint32_t xor_all = 0;
};

// The 32-bit sum and XOR of the big-endian words at window.begin,
// window.begin + 4, ... below window.end, leaving out each word whose address
// is in `skip`. A word past the end of `rom` reads as 0, as in bytes::ReadU32Be.
WordFold FoldU32Be(bytes::ByteView rom, AddressRange window, AddressRange skip = {});

// The 16-bit sum of the big-endian halfwords at window.begin,
// window.begin + 2, ... below window.end.
std::uint16_t SumU16Be(bytes::ByteView rom, AddressRange window);

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
