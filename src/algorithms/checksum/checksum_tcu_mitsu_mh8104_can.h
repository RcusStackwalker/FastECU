#pragma once

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumTcuMitsuMH8104Can
{
  public:
    ChecksumTcuMitsuMH8104Can();
    ~ChecksumTcuMitsuMH8104Can();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);

  private:
};
