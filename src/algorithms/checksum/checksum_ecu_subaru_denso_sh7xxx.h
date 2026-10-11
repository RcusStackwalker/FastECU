#pragma once

#include "checksum_result.h"
#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_image.h"

class ChecksumEcuSubaruDensoSH7xxx
{
  public:
    // The table at `table_address` holds ECU addresses within `rom`'s range.
    static ChecksumResult CalculateChecksumResult(const fastecu::memory::MemoryView& rom,
                                                  fastecu::memory::FlashAddress table_address, uint32_t table_length);
};
