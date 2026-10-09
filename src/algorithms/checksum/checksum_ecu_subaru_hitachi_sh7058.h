#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruHitachiSH7058
{
  public:
    static ChecksumResult CalculateChecksumResult(bytes::ByteView rom_data);
};
