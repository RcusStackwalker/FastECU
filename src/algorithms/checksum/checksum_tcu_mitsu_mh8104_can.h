#ifndef CHECKSUM_TCU_MITSU_MH8104_CAN_H
#define CHECKSUM_TCU_MITSU_MH8104_CAN_H

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

#endif // CHECKSUM_TCU_MITSU_MH8104_CAN_H
