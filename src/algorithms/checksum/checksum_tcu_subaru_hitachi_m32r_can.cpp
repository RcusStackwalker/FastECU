#include "checksum_tcu_subaru_hitachi_m32r_can.h"

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include <array>

namespace
{

// Checksum 2: the sum of every word outside the two fields at 0x8000, stored
// as the bytes below.
std::uint32_t Checksum2Field(bytes::ByteView rom)
{
    const std::uint32_t sum = fastecu::checksum::internal::FoldU32Be(rom.first<0x8000>(), rom.subspan(0x8008)).sum;
    std::array<uint8_t, 4> field_bytes{};
    field_bytes[3] = static_cast<std::uint8_t>(0xffU - ((sum >> 24U) & 0xffU));
    field_bytes[2] = static_cast<std::uint8_t>(0xffU - ((sum >> 16U) & 0xffU));
    field_bytes[1] = static_cast<std::uint8_t>(0xffU - ((sum >> 8U) & 0xffU));
    field_bytes[0] = static_cast<std::uint8_t>(0x100U - (sum & 0xffU)); // 0x100 wraps to 0
    return bytes::ReadU32Le(field_bytes);
}

} // namespace

ChecksumResult ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(bytes::ByteView rom_view)
{
    // Both sums run over the whole file, a 32-bit word at a time, as the
    // legacy module did; the checksum fields sit at fixed addresses
    // 0x8000-0x8007 and 0x8020-0x8023. M32R_512KB files are 0x80000 bytes.
    // Past this check, every piece the sums take is a whole number of words.
    if (rom_view.size() < 0x8024 || rom_view.size() % 4 != 0)
    {
        return {.status = ChecksumResult::Status::kInvalidSize,
                .rom_data = bytes::Bytes(rom_view.begin(), rom_view.end()),
                .message = "ROM size does not match the checksum layout"};
    }
    bytes::Bytes rom_data(rom_view.begin(), rom_view.end());

    constexpr std::size_t kChecksum1BalanceAddress = 0x8020;
    constexpr std::size_t kChecksum2Address = 0x8000;

    bool checksum_ok = true;

    const uint32_t checksum_1_value_calculated =
        fastecu::checksum::internal::FoldU32Be(bytes::ByteView(rom_data).subspan(0x8020)).sum;
    const uint32_t checksum_2_value_calculated = Checksum2Field(rom_data);

    const uint32_t checksum_2_value_stored = bytes::ReadU32Be(rom_data, kChecksum2Address);

    if (checksum_1_value_calculated != 0x5aa5a55a)
    {
        checksum_ok = false;

        fastecu::checksum::internal::RebalanceU32Be(rom_data, kChecksum1BalanceAddress, checksum_1_value_calculated,
                                                    0x5aa5a55a);
    }

    if (checksum_2_value_calculated != checksum_2_value_stored)
    {
        checksum_ok = false;

        // Recomputed: rebalancing checksum 1 above may have changed a word this one sums.
        const uint32_t checksum_2_field = Checksum2Field(rom_data);

        bytes::WriteU32Be(rom_data, kChecksum2Address, checksum_2_field);
        bytes::WriteU32Be(rom_data, kChecksum2Address + 4, checksum_2_field);
    }
    ChecksumResult result;
    result.rom_data = rom_data;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Hitachi M32R K-Line/CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    return result;
}
