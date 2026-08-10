#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fastecu::checksum::internal
{

void rebalanceU16Be(bytes::MutableByteView rom, std::size_t offset,
                    std::uint16_t observed, std::uint16_t target);
void rebalanceU32Be(bytes::MutableByteView rom, std::size_t offset,
                    std::uint32_t observed, std::uint32_t target);

} // namespace fastecu::checksum::internal

namespace fastecu::checksum
{

// One's-complement-style 8-bit checksum: sum bytes, and whenever the
// running sum overflows 8 bits, add 1 back in before truncating (rather
// than a plain mod-256 sum). Used to checksum ECU reflash blocks.
std::uint8_t cks_add8(std::span<const std::uint8_t> data);

// Plain 8-bit additive checksum (sum of bytes, mod 256), optionally
// complemented as 0x100 - sum. Used to checksum request/response frames
// across several ECU communication protocols; not specific to any one of
// them.
std::uint8_t checksum8(bytes::ByteView data, bool dec0x100 = false);

// CRC-32 with the ECU-reflash-specific polynomial 0x5AA5A55A (not the
// standard IEEE 802.3 polynomial). Used to verify a written flash page
// against the CRC the ECU reports back.
std::uint32_t crc32(bytes::ByteView data);
std::uint32_t crc32(const unsigned char *buf, std::uint32_t len);

} // namespace fastecu::checksum
