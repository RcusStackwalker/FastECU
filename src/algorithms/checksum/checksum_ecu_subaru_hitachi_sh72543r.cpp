#include "checksum_ecu_subaru_hitachi_sh72543r.h"
#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(bytes::ByteView romView)
{
    if (romView.size() != 0x200000)
    {
        return {.status = ChecksumResult::Status::InvalidSize,
                .romData = bytes::Bytes(romView.begin(), romView.end()),
                .message = "ROM size does not match the checksum layout"};
    }
    /*******************
     *
     * Checksum is calculated between 0x6000 - 0x1fffff, 16bit summation, balance value is word at 0x1ffffe
     * PTR_DAT_000b446c
     *
     ******************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint16_t chksum = 0;

    for (int i = 0x6000; i < 0x200000; i += 2)
    {
        chksum += bytes::readU16Be(romData, static_cast<std::size_t>(i));
    }

    ChecksumResult result;
    if (chksum != 0x5aa5)
    {
        fastecu::checksum::internal::rebalanceU16Be(romData, 0x1ffffe, chksum, 0x5aa5);

        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi SH72543r ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    result.romData = romData;
    return result;
}
