#ifndef CHECKSUM_TCU_SUBARU_HITACHI_M32R_CAN_H
#define CHECKSUM_TCU_SUBARU_HITACHI_M32R_CAN_H

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

#endif // CHECKSUM_TCU_SUBARU_HITACHI_M32R_CAN_H
