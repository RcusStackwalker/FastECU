#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruHitachiSh72543r
{
  public:
    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);
};
