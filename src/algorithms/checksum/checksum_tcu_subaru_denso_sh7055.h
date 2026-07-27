#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumTcuSubaruDensoSH7055
{
  public:
    ChecksumTcuSubaruDensoSH7055();
    ~ChecksumTcuSubaruDensoSH7055();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);

  private:
};
