#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuMitsuM32rCan
{
  public:
    // `rom_view` holds ECU addresses [0, size): the layout's constants are ECU
    // addresses.
    static ChecksumResult CalculateChecksumResult(bytes::ByteView rom_view);
};
