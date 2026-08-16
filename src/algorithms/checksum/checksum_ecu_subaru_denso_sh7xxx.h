#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruDensoSH7xxx
{
  public:
    // Note that offset is added to all addresses
    static ChecksumResult calculate_checksum_result(bytes::ByteView romData, uint32_t checksum_area_start,
                                                    uint32_t checksum_area_length, int32_t offset = 0);
};
