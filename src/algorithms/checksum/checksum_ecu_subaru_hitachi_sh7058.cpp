#include "checksum_ecu_subaru_hitachi_sh7058.h"
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

ChecksumEcuSubaruHitachiSH7058::ChecksumEcuSubaruHitachiSH7058()
{
}

ChecksumEcuSubaruHitachiSH7058::~ChecksumEcuSubaruHitachiSH7058()
{
}

ChecksumResult ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(bytes::ByteView romView)
{
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
        bytes::Bytes balance_value_array;
        uint32_t balance_value = bytes::readU32Be(romData, checksum_5_balance_value_address);

        balance_value += 0x5aa5a55a - checksum_5_value_calculated;

        bytes::appendU32Be(balance_value_array, balance_value);
        overwriteAt(romData, checksum_5_balance_value_address, balance_value_array);
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

        bytes::Bytes checksum;
        bytes::appendU32Be(checksum, checksum_3_value_calculated);
        overwriteAt(romData, checksum_3_value_address, checksum);

        /*
                QByteArray balance_value_array;
                uint32_t balance_value = ((uint8_t)romData.at(checksum_3_balance_value_address) << 24) + ((uint8_t)romData.at(checksum_3_balance_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_3_balance_value_address + 2) << 8) + ((uint8_t)romData.at(checksum_3_balance_value_address + 3));

                msg.clear();
                msg.append(QString("Balance value before: 0x%1").arg(balance_value,8,16,QLatin1Char('0')).toUtf8());
                qDebug() << msg;

                romData.replace(checksum_3_value_address, 4, "\xC1\x4F\xB6\xC1");
                checksum_3_value_stored = 0xC14FB6C1;
                balance_value += checksum_3_value_stored - checksum_3_value_calculated;

                msg.clear();
                msg.append(QString("Balance value after: 0x%1").arg(balance_value,8,16,QLatin1Char('0')).toUtf8());
                qDebug() << msg;

                balance_value_array.append((uint8_t)((balance_value >> 24) & 0xff));
                balance_value_array.append((uint8_t)((balance_value >> 16) & 0xff));
                balance_value_array.append((uint8_t)((balance_value >> 8) & 0xff));
                balance_value_array.append((uint8_t)(balance_value & 0xff));
                romData.replace(checksum_3_balance_value_address, balance_value_array.length(), balance_value_array);
        */
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

        bytes::Bytes checksum;
        bytes::appendU32Be(checksum, checksum_4_value_calculated);
        overwriteAt(romData, checksum_4_value_address, checksum);
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
