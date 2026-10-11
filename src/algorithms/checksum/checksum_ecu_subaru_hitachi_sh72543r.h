#pragma once

#include <cstdint>
#include <span>

#include "checksum_result.h"

class ChecksumEcuSubaruHitachiSh72543r
{
  public:
    // The fixed layout covers ECU addresses [0, kRomSize); its constants are
    // ECU addresses.
    static constexpr std::uint32_t kRomSize = 0x200000;
    using Rom = std::span<const std::uint8_t, kRomSize>;

    static ChecksumResult CalculateChecksumResult(Rom rom);
};
