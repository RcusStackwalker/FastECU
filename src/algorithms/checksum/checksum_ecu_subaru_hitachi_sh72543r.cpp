#include "checksum_ecu_subaru_hitachi_sh72543r.h"

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumEcuSubaruHitachiSh72543r::CalculateChecksumResult(Rom rom)
{
    const bytes::ByteView rom_view = rom;
    /*******************
     *
     * Checksum is calculated between 0x6000 - 0x1fffff, 16bit summation, balance value is word at 0x1ffffe
     * PTR_DAT_000b446c
     *
     ******************/
    bytes::Bytes rom_data(rom_view.begin(), rom_view.end());

    const uint16_t chksum = fastecu::checksum::internal::SumU16Be({rom_view.subspan(0x6000)});

    ChecksumResult result;
    if (chksum != 0x5aa5)
    {
        fastecu::checksum::internal::RebalanceU16Be(rom_data, 0x1ffffe, chksum, 0x5aa5);

        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Hitachi SH72543r ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    result.rom_data = rom_data;
    return result;
}
