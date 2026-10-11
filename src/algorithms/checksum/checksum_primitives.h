#pragma once

#include "checksum_result.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fastecu::checksum::internal
{

// kInvalidSize with the view's bytes unchanged, unless `rom` starts at ECU
// address 0: the fixed layouts address bytes from 0.
std::optional<ChecksumResult> RequireStartAtZero(const memory::MemoryView& rom);

void RebalanceU16Be(bytes::MutableByteView rom, std::size_t offset, std::uint16_t observed, std::uint16_t target);
void RebalanceU32Be(bytes::MutableByteView rom, std::size_t offset, std::uint32_t observed, std::uint32_t target);

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
