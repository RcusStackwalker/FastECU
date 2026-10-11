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

    uint16_t chksum = 0;

    for (int i = 0x6000; i < 0x200000; i += 2)
    {
        chksum += bytes::ReadU16Be(rom_data, static_cast<std::size_t>(i));
    }

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
