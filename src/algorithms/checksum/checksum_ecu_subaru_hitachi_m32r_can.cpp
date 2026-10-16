#include "checksum_ecu_subaru_hitachi_m32r_can.h"

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(Rom rom)
{
    const bytes::ByteView rom_view = rom;
    /*******************
     *
     *  Checksum 1 calculated between 0x6000 - 0x8000 excluding 0x10000 - 0x10003, 32bit sum, result at 0x7ffe8
     *  Checksum 2 calculated between 0x6000 - 0x8000 excluding 0x10000 - 0x10003, 32 bit XOR, result at 0x7ffec
     *  Checksum 3 calculated between 0x0000 - 0x7ffef excluding 0x10000 - 0x10003, 32bit sum, result at 0x7fff0
     *  Checksum 4 calculated between 0x0000 - 0x7ffef excluding 0x10000 - 0x10003, 32 bit XOR, result at 0x7fff4
     *  Checksum 5 calculated between 0x4000 - 0x7ffff excluding 0x10000 - 0x10003, 16bit sum, must mach 0x5aa5,
     * balancing address 0x7fffa Checksum 6 calculated between 0x0000 - 0x7ffff excluding 0x10000 - 0x10003, 8bit sum &
     * XOR, result 16bit, sum hi byte, XOR low byte
     *
     * ****************/
    bytes::Bytes rom_data(rom_view.begin(), rom_view.end());

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
    const fastecu::checksum::internal::WordFold checksums_1_and_2 =
        fastecu::checksum::internal::FoldU32Be(rom_data, {0x6000, 0x8000});
    checksum_1_value_calculated = checksums_1_and_2.sum;
    checksum_2_value_calculated = checksums_1_and_2.xor_all;
    checksum_1_value_stored = bytes::ReadU32Be(rom_data, checksum_1_value_address);
    checksum_2_value_stored = bytes::ReadU32Be(rom_data, checksum_2_value_address);
    if (checksum_1_value_calculated != checksum_1_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, checksum_1_value_address, checksum_1_value_calculated);
    }
    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, checksum_2_value_address, checksum_2_value_calculated);
    }
    /****************************************
     *
     * Calculate and fix checksums 3 and 4
     *
     * *************************************/
    // Every checksum but 1 and 2 leaves out the word at 0x10000.
    const fastecu::checksum::internal::AddressRange excluded_word{0x10000, 0x10004};
    const fastecu::checksum::internal::WordFold checksums_3_and_4 =
        fastecu::checksum::internal::FoldU32Be(rom_data, {0x0000, 0x7fff0}, excluded_word);
    checksum_3_value_calculated = checksums_3_and_4.sum;
    checksum_4_value_calculated = checksums_3_and_4.xor_all;
    checksum_3_value_stored = bytes::ReadU32Be(rom_data, checksum_3_value_address);
    checksum_4_value_stored = bytes::ReadU32Be(rom_data, checksum_4_value_address);
    if (checksum_3_value_calculated != checksum_3_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, checksum_3_value_address, checksum_3_value_calculated);
    }
    if (checksum_4_value_calculated != checksum_4_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, checksum_4_value_address, checksum_4_value_calculated);
    }
    /****************************************
     *
     * Calculate and fix checksum 5
     *
     * *************************************/
    checksum_5_value_calculated = static_cast<uint16_t>(
        fastecu::checksum::internal::FoldU32Be(rom_data, {0x4000, rom_data.size()}, excluded_word).sum);
    if (checksum_5_value_calculated != 0x5aa5)
    {
        checksum_ok = false;

        fastecu::checksum::internal::RebalanceU16Be(rom_data, checksum_5_balance_value_address,
                                                    checksum_5_value_calculated, 0x5aa5);
    }
    /****************************************
     *
     * Calculate checksum 6
     *
     * *************************************/
    for (int i = 0; i < static_cast<int>(rom_data.size()); i += 1)
    {
        if (i < 0x10000 || i > 0x10003)
        {
            checksum_6_value_hi_calculated += rom_data[static_cast<std::size_t>(i)];
            checksum_6_value_lo_calculated ^= rom_data[static_cast<std::size_t>(i)];
        }
    }
    checksum_6_value_calculated = (checksum_6_value_hi_calculated << 8) + checksum_6_value_lo_calculated;
    checksum_6_value_stored = bytes::ReadU16Be(rom_data, checksum_6_value_address);
    if (checksum_6_value_calculated != checksum_6_value_stored)
    {
        // TODO: Preserve the legacy status quirk until writing the checksum-6
        // field at 0x10000 is validated on supported hardware. A mismatch is
        // reported as Corrected even though this checksum is not written.
        checksum_ok = false;
    }

    ChecksumResult result;
    result.rom_data = rom_data;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Hitachi M32R CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    return result;
}
