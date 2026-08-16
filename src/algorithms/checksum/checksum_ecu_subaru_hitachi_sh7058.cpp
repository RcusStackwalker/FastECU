#include "checksum_ecu_subaru_hitachi_sh7058.h"
#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(bytes::ByteView romView)
{
    // Fixed 1 MiB layout: checksum fields occupy 0xFFFE8-0xFFFFB.
    if (romView.size() != 0x100000)
    {
        return {.status = ChecksumResult::Status::InvalidSize,
                .romData = bytes::Bytes(romView.begin(), romView.end()),
                .message = "ROM size does not match the checksum layout"};
    }
    /*******************
     *
     *  Checksum 1 calculated between 0x18000 - 0x1dfff, 32bit sum, result at 0x7ffe8
     *  Checksum 2 calculated between 0x18000 - 0x1dfff, 32 bit XOR, result at 0x7ffec
     *  Checksum 5 calculated between 0x4000 - 0xfffff excluding 0xffff0 - 0xffff7, 32-bit sum. Balance value is
     *  calculated so that this matches with 0x5aa5a55a.
     *  Checksum 3 calculated between 0x0000 - 0xfffff excluding 0xffff0 - 0xffff7, 32-bit sum, result at 0x7fff0
     *  Checksum 4 calculated between 0x0000 - 0xfffff excluding 0xffff0 - 0xffff7, 32-bit XOR, result at 0x7fff4
     *
     * ****************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint32_t checksum_1_value_stored = 0;
    uint32_t checksum_1_value_calculated = 0;
    uint32_t checksum_1_value_address = 0xfffe8;
    uint32_t checksum_2_value_stored = 0;
    uint32_t checksum_2_value_calculated = 0;
    uint32_t checksum_2_value_address = 0xfffec;
    uint32_t checksum_3_value_stored = 0;
    uint32_t checksum_3_value_calculated = 0;
    uint32_t checksum_3_value_address = 0xffff0;
    uint32_t checksum_4_value_stored = 0;
    uint32_t checksum_4_value_calculated = 0;
    uint32_t checksum_4_value_address = 0xffff4;

    uint32_t checksum_5_value_calculated = 0;
    uint32_t checksum_5_balance_value_address = 0xffff8;

    bool checksum_ok = true;

    /****************************************
     *
     * Calculate and fix checksums 1 and 2
     *
     * *************************************/
    for (int i = 0x18400; i < 0x1e000; i += 4)
    {
        const std::uint32_t word = bytes::readU32Be(romData, static_cast<std::size_t>(i));
        checksum_1_value_calculated += word;
        checksum_2_value_calculated ^= word;
    }
    checksum_1_value_stored = bytes::readU32Be(romData, checksum_1_value_address);
    checksum_2_value_stored = bytes::readU32Be(romData, checksum_2_value_address);
    if (checksum_1_value_calculated != checksum_1_value_stored)
    {
        checksum_ok = false;

        bytes::writeU32Be(romData, checksum_1_value_address, checksum_1_value_calculated);
    }
    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        bytes::writeU32Be(romData, checksum_2_value_address, checksum_2_value_calculated);
    }
    /****************************************
     *
     * Calculate and fix checksum 5
     *
     * *************************************/
    for (uint32_t i = 0x4000; i < (uint32_t)romData.size(); i += 4)
    {
        if (i != checksum_3_value_address && i != checksum_4_value_address)
        {
            checksum_5_value_calculated += bytes::readU32Be(romData, static_cast<std::size_t>(i));
        }
    }
    if (checksum_5_value_calculated != 0x5aa5a55a)
    {
        fastecu::checksum::internal::rebalanceU32Be(romData, checksum_5_balance_value_address,
                                                    checksum_5_value_calculated, 0x5aa5a55a);
    }
    /****************************************
     *
     * Calculate and fix checksums 3 and 4
     *
     * *************************************/
    for (uint32_t i = 0x0000; i < 0x100000; i += 4)
    {
        if (i != checksum_3_value_address && i != checksum_4_value_address)
        {
            const std::uint32_t word2 = bytes::readU32Be(romData, static_cast<std::size_t>(i));
            checksum_3_value_calculated += word2;
            checksum_4_value_calculated ^= word2;
        }
    }
    checksum_3_value_stored = bytes::readU32Be(romData, checksum_3_value_address);
    checksum_4_value_stored = bytes::readU32Be(romData, checksum_4_value_address);
    if (checksum_3_value_calculated != checksum_3_value_stored)
    {
        checksum_ok = false;

        bytes::writeU32Be(romData, checksum_3_value_address, checksum_3_value_calculated);
    }
    if (checksum_4_value_calculated != checksum_4_value_stored)
    {
        checksum_ok = false;

        checksum_4_value_calculated = 0;
        for (uint32_t i = 0x0000; i < 0x100000; i += 4)
        {
            if (i != checksum_3_value_address && i != checksum_4_value_address)
            {
                checksum_4_value_calculated ^= bytes::readU32Be(romData, i);
            }
        }

        bytes::writeU32Be(romData, checksum_4_value_address, checksum_4_value_calculated);
    }

    ChecksumResult result;
    result.romData = romData;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi SH7058 CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
