#ifndef CHECKSUM_ECU_SUBARU_HITACHI_SH72543R_H
#define CHECKSUM_ECU_SUBARU_HITACHI_SH72543R_H

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

#endif // CHECKSUM_ECU_SUBARU_HITACHI_SH72543R_H
