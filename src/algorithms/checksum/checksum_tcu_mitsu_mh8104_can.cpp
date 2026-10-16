#include "checksum_tcu_mitsu_mh8104_can.h"

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumTcuMitsuMH8104Can::CalculateChecksumResult(Rom rom)
{
    const bytes::ByteView rom_view = rom;
    /****************************
     *
     *  FUN_00001578: Check that 0x8000 = 0x5aa5 & 0x7fffe = 0xa55a
     *  FUN_0000288c; Check that
     *
     *
     *
     * *************************/
    bytes::Bytes rom_data(rom_view.begin(), rom_view.end());

    uint32_t checksum_balance_value_address = 0x81fc;
    uint32_t checksum_target = 0x5aa45aab;

    uint32_t checksum = fastecu::checksum::internal::FoldU32Be(rom_data, {0x8000, 0x80000}).sum;

    bool checksum_ok = true;

    checksum -= 0xffff;
    for (int j = 0; j < 5; j++)
    {
        checksum -= 0xffffffff;
    }

    if (checksum != checksum_target)
    {
        checksum_ok = false;

        fastecu::checksum::internal::RebalanceU32Be(rom_data, checksum_balance_value_address, checksum,
                                                    checksum_target);
    }
    ChecksumResult result;
    result.rom_data = rom_data;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Hitachi M32R K-Line/CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    return result;
}
