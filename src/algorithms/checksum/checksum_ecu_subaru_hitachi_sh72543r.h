#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruHitachiSh72543r
{
  public:
    static ChecksumResult CalculateChecksumResult(bytes::ByteView rom_data);
};
