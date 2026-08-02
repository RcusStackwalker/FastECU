#include "checksum_tcu_subaru_hitachi_m32r_can.h"
#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView romView)
{
    if (romView.size() != 0x10000)
    {
        return {.status = ChecksumResult::Status::InvalidSize,
                .romData = bytes::Bytes(romView.begin(), romView.end()),
                .message = "ROM size does not match the checksum layout"};
    }
    bytes::Bytes romData(romView.begin(), romView.end());

    uint32_t checksum_1_value_calculated = 0;
    uint32_t checksum_1_balance_value_address = 0x8020;
    uint32_t checksum_2_value_calculated = 0;
    uint32_t checksum_2_value_stored = 0;
    uint32_t checksum_2_balance_value_address = 0x8000;

    bool checksum_ok = true;

    for (int i = 0x0; i < static_cast<int>(romData.size()); i += 4)
    {
        if (i >= 0x8020)
        {
            checksum_1_value_calculated += bytes::readU32Be(romData, static_cast<std::size_t>(i));
        }
        if (i < 0x8000 || i > 0x8007)
        {
            checksum_2_value_calculated += bytes::readU32Be(romData, static_cast<std::size_t>(i));
        }
    }

    uint8_t checksum_2_value_calculated_bytes[4];
    checksum_2_value_calculated_bytes[3] = 0xff - ((checksum_2_value_calculated >> 24) & 0xff);
    checksum_2_value_calculated_bytes[2] = 0xff - ((checksum_2_value_calculated >> 16) & 0xff);
    checksum_2_value_calculated_bytes[1] = 0xff - ((checksum_2_value_calculated >> 8) & 0xff);
    checksum_2_value_calculated_bytes[0] = 0x100 - (checksum_2_value_calculated & 0xff);
    checksum_2_value_calculated = (checksum_2_value_calculated_bytes[3] << 24) + (checksum_2_value_calculated_bytes[2] << 16) + (checksum_2_value_calculated_bytes[1] << 8) + checksum_2_value_calculated_bytes[0];

    checksum_2_value_stored = bytes::readU32Be(romData, checksum_2_balance_value_address);

    if (checksum_1_value_calculated != 0x5aa5a55a)
    {
        checksum_ok = false;

        fastecu::checksum::internal::rebalanceU32Be(
            romData, checksum_1_balance_value_address, checksum_1_value_calculated, 0x5aa5a55a);
    }

    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        checksum_2_value_calculated = 0;
        for (int i = 0x0; i < static_cast<int>(romData.size()); i += 4)
        {
            if (i < 0x8000 || i > 0x8007)
            {
                checksum_2_value_calculated += bytes::readU32Be(romData, static_cast<std::size_t>(i));
            }
        }
        uint8_t checksum_2_value_calculated_bytes2[4];
        checksum_2_value_calculated_bytes2[3] = 0xff - ((checksum_2_value_calculated >> 24) & 0xff);
        checksum_2_value_calculated_bytes2[2] = 0xff - ((checksum_2_value_calculated >> 16) & 0xff);
        checksum_2_value_calculated_bytes2[1] = 0xff - ((checksum_2_value_calculated >> 8) & 0xff);
        checksum_2_value_calculated_bytes2[0] = 0x100 - (checksum_2_value_calculated & 0xff);
        checksum_2_value_calculated = (checksum_2_value_calculated_bytes2[3] << 24) + (checksum_2_value_calculated_bytes2[2] << 16) + (checksum_2_value_calculated_bytes2[1] << 8) + checksum_2_value_calculated_bytes2[0];

        bytes::writeU32Be(romData, checksum_2_balance_value_address, checksum_2_value_calculated);
        bytes::writeU32Be(romData, checksum_2_balance_value_address + 4, checksum_2_value_calculated);
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
