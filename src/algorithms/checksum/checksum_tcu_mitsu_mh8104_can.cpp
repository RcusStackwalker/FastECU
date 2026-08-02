#include "checksum_tcu_mitsu_mh8104_can.h"
#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumTcuMitsuMH8104Can::calculate_checksum_result(bytes::ByteView romView)
{
    if (romView.size() != 0x80000)
    {
        return {.status = ChecksumResult::Status::InvalidSize,
                .romData = bytes::Bytes(romView.begin(), romView.end()),
                .message = "ROM size does not match the checksum layout"};
    }
    /****************************
     *
     *  FUN_00001578: Check that 0x8000 = 0x5aa5 & 0x7fffe = 0xa55a
     *  FUN_0000288c; Check that
     *
     *
     *
     * *************************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint32_t checksum_balance_value_address = 0x81fc;
    uint32_t checksum_target = 0x5aa45aab;

    uint32_t checksum = 0;

    bool checksum_ok = true;

    for (int i = 0x8000; i < 0x80000; i += 4)
    {
        checksum += bytes::readU32Be(romData, static_cast<std::size_t>(i));
    }
    checksum -= 0xffff;
    for (int j = 0; j < 5; j++)
    {
        checksum -= 0xffffffff;
    }

    if (checksum != checksum_target)
    {
        checksum_ok = false;

        fastecu::checksum::internal::rebalanceU32Be(
            romData, checksum_balance_value_address, checksum, checksum_target);
    }
    ChecksumResult result;
    result.romData = romData;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi M32R K-Line/CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
