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
    // Each checksum below reads the corrections written before it.
    const Rom image{rom_data};

    constexpr std::size_t kChecksum1Address = 0x7ffe8;
    constexpr std::size_t kChecksum2Address = 0x7ffec;
    constexpr std::size_t kChecksum3Address = 0x7fff0;
    constexpr std::size_t kChecksum4Address = 0x7fff4;
    constexpr std::size_t kChecksum5BalanceAddress = 0x7fffa;
    constexpr std::size_t kChecksum6Address = 0x10000;

    bool checksum_ok = true;

    /****************************************
     *
     * Calculate and fix checksums 1 and 2
     *
     * *************************************/
    const auto [checksum_1_value_calculated, checksum_2_value_calculated] =
        fastecu::checksum::internal::FoldU32Be(image.subspan<0x6000, 0x2000>());
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
     * Calculate and fix checksums 3 and 4
     *
     * *************************************/
    // Every checksum but 1 and 2 leaves out the word at 0x10000.
    const auto [checksum_3_value_calculated, checksum_4_value_calculated] =
        fastecu::checksum::internal::FoldU32Be(image.first<0x10000>(), image.subspan<0x10004, 0x6ffec>());
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

        bytes::WriteU32Be(rom_data, kChecksum4Address, checksum_4_value_calculated);
    }
    /****************************************
     *
     * Calculate and fix checksum 5
     *
     * *************************************/
    const auto checksum_5_value_calculated = static_cast<uint16_t>(
        fastecu::checksum::internal::FoldU32Be(image.subspan<0x4000, 0xc000>(), image.subspan<0x10004>()).sum);
    if (checksum_5_value_calculated != 0x5aa5)
    {
        checksum_ok = false;

        fastecu::checksum::internal::RebalanceU16Be(rom_data, kChecksum5BalanceAddress, checksum_5_value_calculated,
                                                    0x5aa5);
    }
    /****************************************
     *
     * Calculate checksum 6
     *
     * *************************************/
    uint8_t checksum_6_value_hi_calculated = 0;
    uint8_t checksum_6_value_lo_calculated = 0;
    for (int i = 0; i < static_cast<int>(rom_data.size()); i += 1)
    {
        if (i < 0x10000 || i > 0x10003)
        {
            checksum_6_value_hi_calculated += rom_data[static_cast<std::size_t>(i)];
            checksum_6_value_lo_calculated ^= rom_data[static_cast<std::size_t>(i)];
        }
    }
    const uint16_t checksum_6_value_calculated = (checksum_6_value_hi_calculated << 8) + checksum_6_value_lo_calculated;
    const uint16_t checksum_6_value_stored = bytes::ReadU16Be(rom_data, kChecksum6Address);
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
