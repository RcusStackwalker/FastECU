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

} // namespace fastecu::checksum
