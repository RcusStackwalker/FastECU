#pragma once

#include "checksum_result.h"
#include "src/algorithms/memory/memory_image.h"

class ChecksumEcuSubaruHitachiSh72543r
{
  public:
    // `rom` must start at ECU address 0: the layout's constants are ECU addresses.
    static ChecksumResult CalculateChecksumResult(const fastecu::memory::MemoryView& rom);
};
