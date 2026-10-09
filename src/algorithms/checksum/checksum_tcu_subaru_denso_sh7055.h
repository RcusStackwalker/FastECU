#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumTcuSubaruDensoSH7055
{
  public:
    static ChecksumResult CalculateChecksumResult(bytes::ByteView rom_data);
};
