#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruHitachiM32rKline
{
  public:
    ChecksumEcuSubaruHitachiM32rKline();
    ~ChecksumEcuSubaruHitachiM32rKline();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);

  private:
};
