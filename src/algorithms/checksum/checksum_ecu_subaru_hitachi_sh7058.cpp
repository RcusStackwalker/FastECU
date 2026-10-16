#include "checksum_ecu_subaru_hitachi_sh7058.h"

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

ChecksumResult ChecksumEcuSubaruHitachiSH7058::CalculateChecksumResult(Rom rom)
{
    const bytes::ByteView rom_view = rom;
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
    bytes::Bytes rom_data(rom_view.begin(), rom_view.end());
    // Each checksum below reads the corrections written before it.
    const bytes::ByteView image = rom_data;

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
    const fastecu::checksum::internal::WordFold checksums_1_and_2 =
        fastecu::checksum::internal::FoldU32Be({image.subspan(0x18400, 0x5c00)});
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
     * Calculate and fix checksum 5
     *
     * *************************************/
    // Checksums 3 to 5 leave out the slots of checksums 3 and 4, 0xffff0 to 0xffff8.
    checksum_5_value_calculated =
        fastecu::checksum::internal::FoldU32Be({image.subspan(0x4000, 0xfbff0), image.subspan(0xffff8)}).sum;
    if (checksum_5_value_calculated != 0x5aa5a55a)
    {
        fastecu::checksum::internal::RebalanceU32Be(rom_data, checksum_5_balance_value_address,
                                                    checksum_5_value_calculated, 0x5aa5a55a);
    }
    /****************************************
     *
     * Calculate and fix checksums 3 and 4
     *
     * *************************************/
    const fastecu::checksum::internal::WordFold checksums_3_and_4 =
        fastecu::checksum::internal::FoldU32Be({image.first(0xffff0), image.subspan(0xffff8)});
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

        // Writing checksum 3 above leaves this value as it was: its slot is
        // one of the two left out.
        bytes::WriteU32Be(rom_data, checksum_4_value_address, checksum_4_value_calculated);
    }

    ChecksumResult result;
    result.rom_data = rom_data;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Hitachi SH7058 CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    return result;
}
