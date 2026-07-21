#ifndef CHECKSUM_ECU_SUBARU_HITACHI_M32R_CAN_H
#define CHECKSUM_ECU_SUBARU_HITACHI_M32R_CAN_H

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruHitachiM32rCan
{
  public:
    ChecksumEcuSubaruHitachiM32rCan();
    ~ChecksumEcuSubaruHitachiM32rCan();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData);

  private:
};

#endif // CHECKSUM_ECU_SUBARU_HITACHI_M32R_CAN_H
