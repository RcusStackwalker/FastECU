#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>

namespace fastecu::checksum::internal
{

void rebalanceU16Be(bytes::MutableByteView rom, std::size_t offset,
                    std::uint16_t observed, std::uint16_t target);
void rebalanceU32Be(bytes::MutableByteView rom, std::size_t offset,
                    std::uint32_t observed, std::uint32_t target);

} // namespace fastecu::checksum::internal
