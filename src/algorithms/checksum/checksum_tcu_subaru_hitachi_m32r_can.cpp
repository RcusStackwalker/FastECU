#include "checksum_tcu_subaru_hitachi_m32r_can.h"
#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include <array>

ChecksumResult ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView romView)
{
    // Fixed 64 KiB M3779x/M3775x layout; checksum fields begin at 0x8000.
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

    std::array<uint8_t, 4> checksum_2_value_calculated_bytes{};
    checksum_2_value_calculated_bytes[3] = 0xffU - ((checksum_2_value_calculated >> 24U) & 0xffU);
    checksum_2_value_calculated_bytes[2] = 0xffU - ((checksum_2_value_calculated >> 16U) & 0xffU);
    checksum_2_value_calculated_bytes[1] = 0xffU - ((checksum_2_value_calculated >> 8U) & 0xffU);
    checksum_2_value_calculated_bytes[0] = 0x100U - (checksum_2_value_calculated & 0xffU);
    checksum_2_value_calculated = bytes::readU32Le(checksum_2_value_calculated_bytes);

    checksum_2_value_stored = bytes::readU32Be(romData, checksum_2_balance_value_address);

    if (checksum_1_value_calculated != 0x5aa5a55a)
    {
        checksum_ok = false;

        fastecu::checksum::internal::rebalanceU32Be(romData, checksum_1_balance_value_address,
                                                    checksum_1_value_calculated, 0x5aa5a55a);
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
        std::array<uint8_t, 4> checksum_2_value_calculated_bytes2{};
        checksum_2_value_calculated_bytes2[3] = 0xffU - ((checksum_2_value_calculated >> 24U) & 0xffU);
        checksum_2_value_calculated_bytes2[2] = 0xffU - ((checksum_2_value_calculated >> 16U) & 0xffU);
        checksum_2_value_calculated_bytes2[1] = 0xffU - ((checksum_2_value_calculated >> 8U) & 0xffU);
        checksum_2_value_calculated_bytes2[0] = 0x100U - (checksum_2_value_calculated & 0xffU);
        checksum_2_value_calculated = bytes::readU32Le(checksum_2_value_calculated_bytes2);

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
