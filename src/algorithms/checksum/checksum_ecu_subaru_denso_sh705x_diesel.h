#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruDensoSH705xDiesel
{
  public:
    static ChecksumResult calculate_checksum_result(bytes::ByteView rom_data, uint32_t checksum_area_start,
                                                    uint32_t checksum_area_length);
};
