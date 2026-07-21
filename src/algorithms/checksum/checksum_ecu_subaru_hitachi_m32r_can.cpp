#include "checksum_ecu_subaru_hitachi_m32r_can.h"
#include "src/algorithms/protocol/bytes.h"

#include <algorithm>

namespace
{

// Mirrors QByteArray::replace(pos, len, payload) for the same-size,
// in-bounds overwrites this file performs (len == payload.size() at every
// call site).
void overwriteAt(bytes::Bytes& data, std::size_t pos, bytes::ByteView payload)
{
    if (pos >= data.size())
    {
        return;
    }
    const std::size_t count = std::min(payload.size(), data.size() - pos);
    std::copy_n(payload.begin(), count, data.begin() + static_cast<std::ptrdiff_t>(pos));
}

} // namespace

ChecksumEcuSubaruHitachiM32rCan::ChecksumEcuSubaruHitachiM32rCan()
{
}

ChecksumEcuSubaruHitachiM32rCan::~ChecksumEcuSubaruHitachiM32rCan()
{
}

ChecksumResult ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView romView)
{
    /*******************
     *
     *  Checksum 1 calculated between 0x6000 - 0x8000 excluding 0x10000 - 0x10003, 32bit sum, result at 0x7ffe8
     *  Checksum 2 calculated between 0x6000 - 0x8000 excluding 0x10000 - 0x10003, 32 bit XOR, result at 0x7ffec
     *  Checksum 3 calculated between 0x0000 - 0x7ffef excluding 0x10000 - 0x10003, 32bit sum, result at 0x7fff0
     *  Checksum 4 calculated between 0x0000 - 0x7ffef excluding 0x10000 - 0x10003, 32 bit XOR, result at 0x7fff4
     *  Checksum 5 calculated between 0x4000 - 0x7ffff excluding 0x10000 - 0x10003, 16bit sum, must mach 0x5aa5, balancing address 0x7fffa
     *  Checksum 6 calculated between 0x0000 - 0x7ffff excluding 0x10000 - 0x10003, 8bit sum & XOR, result 16bit, sum hi byte, XOR low byte
     *
     * ****************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint32_t checksum_1_value_stored = 0;
    uint32_t checksum_1_value_calculated = 0;
    uint32_t checksum_1_value_address = 0x7ffe8;
    uint32_t checksum_2_value_stored = 0;
    uint32_t checksum_2_value_calculated = 0;
    uint32_t checksum_2_value_address = 0x7ffec;
    uint32_t checksum_3_value_stored = 0;
    uint32_t checksum_3_value_calculated = 0;
    uint32_t checksum_3_value_address = 0x7fff0;
    uint32_t checksum_4_value_stored = 0;
    uint32_t checksum_4_value_calculated = 0;
    uint32_t checksum_4_value_address = 0x7fff4;

    uint16_t checksum_5_value_calculated = 0;
    uint16_t checksum_5_balance_value_stored = 0;
    uint32_t checksum_5_balance_value_address = 0x7fffa;

    uint16_t checksum_6_value_stored = 0;
    uint16_t checksum_6_value_calculated = 0;
    uint32_t checksum_6_value_address = 0x10000;
    uint8_t checksum_6_value_hi_calculated = 0;
    uint8_t checksum_6_value_lo_calculated = 0;

    bool checksum_ok = true;

    /****************************************
     *
     * Calculate and fix checksums 1 and 2
     *
     * *************************************/
    for (int i = 0x6000; i < 0x8000; i += 4)
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

        bytes::Bytes checksum;
        bytes::appendU32Be(checksum, checksum_1_value_calculated);
        overwriteAt(romData, checksum_1_value_address, checksum);
    }
    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        bytes::Bytes checksum;
        bytes::appendU32Be(checksum, checksum_2_value_calculated);
        overwriteAt(romData, checksum_2_value_address, checksum);
    }
    /****************************************
     *
     * Calculate and fix checksums 3 and 4
     *
     * *************************************/
    for (int i = 0x0000; i < static_cast<int>(romData.size()); i += 4)
    {
        if (i < 0x10000 || (i > 0x10003 && i < 0x7fff0))
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

        bytes::Bytes checksum;
        bytes::appendU32Be(checksum, checksum_3_value_calculated);
        overwriteAt(romData, checksum_3_value_address, checksum);
    }
    if (checksum_4_value_calculated != checksum_4_value_stored)
    {
        checksum_ok = false;

        bytes::Bytes checksum;
        bytes::appendU32Be(checksum, checksum_4_value_calculated);
        overwriteAt(romData, checksum_4_value_address, checksum);
    }
    /****************************************
     *
     * Calculate and fix checksum 5
     *
     * *************************************/
    for (int i = 0x4000; i < static_cast<int>(romData.size()); i += 4)
    {
        if (i < 0x10000 || i > 0x10003)
        {
            checksum_5_value_calculated += bytes::readU32Be(romData, static_cast<std::size_t>(i));
        }
    }
    checksum_5_balance_value_stored = bytes::readU16Be(romData, checksum_5_balance_value_address);

    if (checksum_5_value_calculated != 0x5aa5)
    {
        checksum_ok = false;

        bytes::Bytes balance_value_array;
        // Reads the same bytes as checksum_5_balance_value_stored above via the same
        // unsigned interpretation; the original here used a sign-extending char cast
        // that could disagree with that stored read for balance bytes >= 0x80.
        uint16_t balance_value = bytes::readU16Be(romData, checksum_5_balance_value_address);

        balance_value += 0x5aa5 - checksum_5_value_calculated;

        bytes::appendU16Be(balance_value_array, balance_value);
        overwriteAt(romData, checksum_5_balance_value_address, balance_value_array);
    }
    /****************************************
     *
     * Calculate checksum 6
     *
     * *************************************/
    for (int i = 0; i < static_cast<int>(romData.size()); i += 1)
    {
        if (i < 0x10000 || i > 0x10003)
        {
            checksum_6_value_hi_calculated += romData[static_cast<std::size_t>(i)];
            checksum_6_value_lo_calculated ^= romData[static_cast<std::size_t>(i)];
        }
    }
    checksum_6_value_calculated = (checksum_6_value_hi_calculated << 8) + checksum_6_value_lo_calculated;
    checksum_6_value_stored = bytes::readU16Be(romData, checksum_6_value_address);
    if (checksum_6_value_calculated != checksum_6_value_stored)
    {
        checksum_ok = false;

        bytes::Bytes checksum;
        bytes::appendU16Be(checksum, checksum_6_value_calculated);
        // overwriteAt(romData, checksum_6_value_address, checksum);
    }
    else
    {
    }

    ChecksumResult result;
    result.romData = romData;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi M32R CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
