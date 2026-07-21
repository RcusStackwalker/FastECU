#ifndef CHECKSUM_ECU_SUBARU_HITACHI_SH7058_H
#define CHECKSUM_ECU_SUBARU_HITACHI_SH7058_H

#include "checksum_result.h"

#include <QDebug>
#include <QObject>

class ChecksumEcuSubaruHitachiSH7058
{
  public:
    ChecksumEcuSubaruHitachiSH7058();
    ~ChecksumEcuSubaruHitachiSH7058();

    static ChecksumResult calculate_checksum_result(QByteArray romData);

  private:
};

#endif // CHECKSUM_ECU_SUBARU_HITACHI_SH7058_H
