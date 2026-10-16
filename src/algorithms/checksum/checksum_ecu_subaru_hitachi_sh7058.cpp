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
    const Rom image{rom_data};

    constexpr std::size_t kChecksum1Address = 0xfffe8;
    constexpr std::size_t kChecksum2Address = 0xfffec;
    constexpr std::size_t kChecksum3Address = 0xffff0;
    constexpr std::size_t kChecksum4Address = 0xffff4;
    constexpr std::size_t kChecksum5BalanceAddress = 0xffff8;

    bool checksum_ok = true;

    /****************************************
     *
     * Calculate and fix checksums 1 and 2
     *
     * *************************************/
    const auto [checksum_1_value_calculated, checksum_2_value_calculated] =
        fastecu::checksum::internal::FoldU32Be(image.subspan<0x18400, 0x5c00>());
    const uint32_t checksum_1_value_stored = bytes::ReadU32Be(rom_data, kChecksum1Address);
    const uint32_t checksum_2_value_stored = bytes::ReadU32Be(rom_data, kChecksum2Address);
    if (checksum_1_value_calculated != checksum_1_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, kChecksum1Address, checksum_1_value_calculated);
    }
    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, kChecksum2Address, checksum_2_value_calculated);
    }
    /****************************************
     *
     * Calculate and fix checksum 5
     *
     * *************************************/
    // Checksums 3 to 5 leave out the slots of checksums 3 and 4, 0xffff0 to 0xffff8.
    const uint32_t checksum_5_value_calculated =
        fastecu::checksum::internal::FoldU32Be(image.subspan<0x4000, 0xfbff0>(), image.subspan<0xffff8>()).sum;
    if (checksum_5_value_calculated != 0x5aa5a55a)
    {
        fastecu::checksum::internal::RebalanceU32Be(rom_data, kChecksum5BalanceAddress, checksum_5_value_calculated,
                                                    0x5aa5a55a);
    }
    /****************************************
     *
     * Calculate and fix checksums 3 and 4
     *
     * *************************************/
    const auto [checksum_3_value_calculated, checksum_4_value_calculated] =
        fastecu::checksum::internal::FoldU32Be(image.first<0xffff0>(), image.subspan<0xffff8>());
    const uint32_t checksum_3_value_stored = bytes::ReadU32Be(rom_data, kChecksum3Address);
    const uint32_t checksum_4_value_stored = bytes::ReadU32Be(rom_data, kChecksum4Address);
    if (checksum_3_value_calculated != checksum_3_value_stored)
    {
        checksum_ok = false;

        bytes::WriteU32Be(rom_data, kChecksum3Address, checksum_3_value_calculated);
    }
    if (checksum_4_value_calculated != checksum_4_value_stored)
    {
        checksum_ok = false;

        // Writing checksum 3 above leaves this value as it was: its slot is
        // one of the two left out.
        bytes::WriteU32Be(rom_data, kChecksum4Address, checksum_4_value_calculated);
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
