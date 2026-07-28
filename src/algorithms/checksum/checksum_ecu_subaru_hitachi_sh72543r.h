#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruHitachiSh72543r
{
  public:
    ChecksumEcuSubaruHitachiSh72543r();
    ~ChecksumEcuSubaruHitachiSh72543r();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);

  private:
};
