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

    constexpr std::size_t kChecksumBalanceAddress = 0x81fc;
    constexpr uint32_t kChecksumTarget = 0x5aa45aab;

    // The legacy code subtracted 0xffffffff five times, which modulo 2^32
    // adds 5.
    const uint32_t checksum = fastecu::checksum::internal::FoldU32Be(rom.subspan<0x8000>()).sum - 0xffffU + 5U;

    bool checksum_ok = true;

    if (checksum != kChecksumTarget)
    {
        checksum_ok = false;

        fastecu::checksum::internal::RebalanceU32Be(rom_data, kChecksumBalanceAddress, checksum, kChecksumTarget);
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
