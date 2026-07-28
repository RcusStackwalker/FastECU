#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumTcuSubaruHitachiM32rCan
{
  public:
    ChecksumTcuSubaruHitachiM32rCan();
    ~ChecksumTcuSubaruHitachiM32rCan();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);

  private:
};
