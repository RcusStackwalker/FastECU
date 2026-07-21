#include "checksum_ecu_subaru_hitachi_m32r_kline.h"
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

ChecksumEcuSubaruHitachiM32rKline::ChecksumEcuSubaruHitachiM32rKline()
{
}

ChecksumEcuSubaruHitachiM32rKline::~ChecksumEcuSubaruHitachiM32rKline()
{
}

ChecksumResult ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::ByteView romView)
{
    /*******************
     *
     *  Checksum 1 calculated between 0x0000 - 0x7ffff excluding 0x8100 - 0x8101, 8bit SUM, result at 0x8100
     *  Checksum 2 calculated between 0x0000 - 0x7ffff excluding 0x8100 - 0x8101, 8bit XOR, result at 0x8101
     *  Checksum 3 calculated between 0x4000 - 0x7ffff excluding 0x8100 - 0x8103, 32bit sum, 16bit result must mach 0x5aa5, balancing address 0x7fffa
     *
     * ****************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint8_t checksum_1_value_stored = 0;
    uint8_t checksum_1_value_calculated = 0;
    uint32_t checksum_1_value_address = 0x8100;
    uint8_t checksum_2_value_stored = 0;
    uint8_t checksum_2_value_calculated = 0;
    uint32_t checksum_2_value_address = 0x8101;

    uint16_t checksum_3_value_calculated = 0;
    uint16_t checksum_3_balance_value_stored = 0;
    uint32_t checksum_3_balance_value_address = 0x7fffa;

    bool checksum_ok = true;

    /****************************************
     *
     * Calculate and fix checksums 1 and 2
     *
     * *************************************/
    for (int i = 0x0000; i < static_cast<int>(romData.size()); i += 1)
    {
        if (i < 0x8100 || i > 0x8101)
        {
            checksum_1_value_calculated += romData[static_cast<std::size_t>(i)];
            checksum_2_value_calculated ^= romData[static_cast<std::size_t>(i)];
        }
    }
    checksum_1_value_stored = romData[checksum_1_value_address];
    checksum_2_value_stored = romData[checksum_2_value_address];
    if (checksum_1_value_calculated != checksum_1_value_stored)
    {
        checksum_ok = false;

        bytes::Bytes checksum;
        checksum.push_back(checksum_1_value_calculated);
        overwriteAt(romData, checksum_1_value_address, checksum);
    }
    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        bytes::Bytes checksum;
        checksum.push_back(checksum_2_value_calculated);
        overwriteAt(romData, checksum_2_value_address, checksum);
    }

    /****************************************
     *
     * Calculate and fix checksum 5
     *
     * *************************************/
    for (int i = 0x4000; i < static_cast<int>(romData.size()); i += 4)
    {
        if (i < 0x8100 || i > 0x8103)
        {
            checksum_3_value_calculated += bytes::readU32Be(romData, static_cast<std::size_t>(i));
        }
    }
    checksum_3_balance_value_stored = bytes::readU16Be(romData, checksum_3_balance_value_address);
    if (checksum_3_value_calculated != 0x5aa5)
    {
        checksum_ok = false;

        bytes::Bytes balance_value_array;
        // Reads the same bytes as checksum_3_balance_value_stored above via the same
        // unsigned interpretation; the original here used a sign-extending char cast
        // that could disagree with that stored read for balance bytes >= 0x80.
        uint16_t balance_value = bytes::readU16Be(romData, checksum_3_balance_value_address);

        balance_value += 0x5aa5 - checksum_3_value_calculated;

        bytes::appendU16Be(balance_value_array, balance_value);
        overwriteAt(romData, checksum_3_balance_value_address, balance_value_array);
    }

    ChecksumResult result;
    result.romData = romData;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi M32R K-Line ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
