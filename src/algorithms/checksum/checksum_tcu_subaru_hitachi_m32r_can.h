#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumTcuSubaruHitachiM32rCan
{
  public:
    static ChecksumResult CalculateChecksumResult(bytes::ByteView rom_data);
};
