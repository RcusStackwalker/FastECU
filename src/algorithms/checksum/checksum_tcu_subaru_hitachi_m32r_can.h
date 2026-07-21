#ifndef CHECKSUM_TCU_SUBARU_HITACHI_M32R_CAN_H
#define CHECKSUM_TCU_SUBARU_HITACHI_M32R_CAN_H

#include "checksum_result.h"

#include <QDebug>
#include <QObject>

class ChecksumTcuSubaruHitachiM32rCan
{
  public:
    ChecksumTcuSubaruHitachiM32rCan();
    ~ChecksumTcuSubaruHitachiM32rCan();

    static ChecksumResult calculate_checksum_result(QByteArray romData);

  private:
};

#endif // CHECKSUM_TCU_SUBARU_HITACHI_M32R_CAN_H
