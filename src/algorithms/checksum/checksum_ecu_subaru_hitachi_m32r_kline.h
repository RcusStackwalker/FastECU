#ifndef CHECKSUM_ECU_SUBARU_HITACHI_M32R_KLINE_H
#define CHECKSUM_ECU_SUBARU_HITACHI_M32R_KLINE_H

#include "checksum_result.h"

#include <QDebug>
#include <QObject>

class ChecksumEcuSubaruHitachiM32rKline
{
  public:
    ChecksumEcuSubaruHitachiM32rKline();
    ~ChecksumEcuSubaruHitachiM32rKline();

    static ChecksumResult calculate_checksum_result(QByteArray romData);

  private:
};

#endif // CHECKSUM_ECU_SUBARU_HITACHI_M32R_KLINE_H
