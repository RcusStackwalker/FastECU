#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruDensoSH7xxx
{
  public:
    // Note that offset is added to all addresses
    static ChecksumResult CalculateChecksumResult(bytes::ByteView rom_data, uint32_t checksum_area_start,
                                                  uint32_t checksum_area_length, int32_t offset = 0);
};
