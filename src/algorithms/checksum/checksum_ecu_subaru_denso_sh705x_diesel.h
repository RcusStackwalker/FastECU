#ifndef CHECKSUM_ECU_SUBARU_DENSO_SH705X_DIESEL_H
#define CHECKSUM_ECU_SUBARU_DENSO_SH705X_DIESEL_H

#include "checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

class ChecksumEcuSubaruDensoSH705xDiesel
{
  public:
    ChecksumEcuSubaruDensoSH705xDiesel();
    ~ChecksumEcuSubaruDensoSH705xDiesel();

    static ChecksumResult calculate_checksum_result(bytes::ByteView romData, uint32_t checksum_area_start, uint32_t checksum_area_length);

  private:
};

#endif // CHECKSUM_ECU_SUBARU_DENSO_SH705X_DIESEL_H
