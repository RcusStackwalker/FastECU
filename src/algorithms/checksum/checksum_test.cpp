#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh705x_diesel.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh7xxx.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_can.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_kline.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh7058.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh72543r.h"
#include "src/algorithms/checksum/checksum_tcu_mitsu_mh8104_can.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_denso_sh7055.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_hitachi_m32r_can.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <utility>
#include <vector>

// Portable-type mirror of tests/test_checksum_results.cpp -- same input and
// expected-output byte vectors for all nine checksum families (the frozen
// Qt contract), expressed with bytes::Bytes/bytes::ByteView instead of
// QByteArray/QString, exercised again here so this package's own test
// target proves the portable API round-trips (math included) without
// depending on //tests or linking Qt.

namespace
{

void compareChangedBytes(const bytes::Bytes& original,
                         const bytes::Bytes& actual,
                         const std::vector<std::pair<std::size_t, bytes::Bytes>>& replacements)
{
    bytes::Bytes expected = original;
    for (const auto& replacement : replacements)
    {
        const std::size_t pos = replacement.first;
        const bytes::Bytes& payload = replacement.second;
        ASSERT_LE(pos, expected.size());
        const std::size_t count = std::min(payload.size(), expected.size() - pos);
        std::copy_n(payload.begin(), count, expected.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    EXPECT_EQ(actual, expected);
}

// Mirrors the Qt test file's densoRomWithChecksumTable(): 4 zero bytes,
// then dword 0x00000001 (the value the checksum block will sum), then 8
// zero bytes of padding, then the {addr_lo=4, addr_hi=8, diff=stored}
// checksum record at offset 16 -- 28 bytes total.
bytes::Bytes densoRomWithChecksumTable(std::uint32_t storedChecksum)
{
    bytes::Bytes rom(4, 0x00);
    bytes::appendU32Be(rom, 0x00000001);
    rom.resize(rom.size() + 8, 0x00);
    bytes::appendU32Be(rom, 0x00000004);
    bytes::appendU32Be(rom, 0x00000008);
    bytes::appendU32Be(rom, storedChecksum);
    return rom;
}

} // namespace

TEST(ChecksumPortable, DensoSh705xDieselCorrectsSingleZeroRecord)
{
    const bytes::Bytes original(12, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(bytes::ByteView(original), 0, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Denso SH705x Checksum");
    compareChangedBytes(
        original, result.romData,
        {{0, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa5, 0xa5, 0x5a}}});
}

TEST(ChecksumPortable, DensoSh705xDieselCorrectedRecordTriggersDisabledCompatibility)
{
    const bytes::Bytes original = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa5, 0xa5, 0x5a};

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(bytes::ByteView(original), 0, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::Disabled);
    EXPECT_EQ(result.message, "ROM has all checksums disabled");
    // Matches the legacy calculate_checksum()'s `return 0;` on this path:
    // romData comes back empty rather than the original ROM bytes.
    EXPECT_TRUE(result.romData.empty());
}

TEST(ChecksumPortable, DensoSh705xDieselKeepsMatchingRecord)
{
    const bytes::Bytes original = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x5a, 0xa5, 0xa5, 0x52};

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(bytes::ByteView(original), 0, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(result.romData, original);
}

TEST(ChecksumPortable, DensoSh7xxxReturnsUnchangedForMatchingChecksum)
{
    const bytes::Bytes rom = densoRomWithChecksumTable(0x5aa5a559);

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView(rom), 16, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(result.romData, rom);
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(result.changed());
}

TEST(ChecksumPortable, DensoSh7xxxReturnsCorrectedDataForMismatchedChecksum)
{
    const bytes::Bytes rom = densoRomWithChecksumTable(0x00000000);

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView(rom), 16, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    bytes::Bytes expected = rom;
    const bytes::Bytes correctedChecksum = {0x5a, 0xa5, 0xa5, 0x59};
    std::copy(correctedChecksum.begin(), correctedChecksum.end(), expected.begin() + 24);
    EXPECT_EQ(result.romData, expected);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.changed());

    const ChecksumResult unchangedResult =
        ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView(result.romData), 16, 12);
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, result.romData);
}

TEST(ChecksumPortable, DensoSh7xxxReturnsDisabledWithoutClearingRomData)
{
    bytes::Bytes rom(16, 0x00);
    bytes::appendU32Be(rom, 0x00000000);
    bytes::appendU32Be(rom, 0x00000000);
    bytes::appendU32Be(rom, 0x5aa5a55a);

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView(rom), 16, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::Disabled);
    EXPECT_EQ(result.romData, rom);
    EXPECT_TRUE(result.ok());
}

TEST(ChecksumPortable, DensoSh7xxxRejectsChecksumAreaOutsideRom)
{
    const bytes::Bytes rom(16, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView(rom), 16, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::InvalidSize);
    EXPECT_EQ(result.romData, rom);
    EXPECT_FALSE(result.ok());
}

TEST(ChecksumPortable, DensoSh7xxxRejectsNonTableAlignedChecksumArea)
{
    const bytes::Bytes rom(16, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView(rom), 0, 10);

    EXPECT_EQ(result.status, ChecksumResult::Status::ParseError);
    EXPECT_EQ(result.romData, rom);
    EXPECT_FALSE(result.ok());
}

TEST(ChecksumPortable, HitachiM32rCanBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R CAN ECU Checksum");
    compareChangedBytes(original, result.romData, {{0x7fffa, bytes::Bytes{0x5a, 0xa5}}});

    // Checksum 6's mismatch handling never writes the fix back to romData
    // (dead code in the algorithm, unchanged by this task), so checksum_ok
    // stays false forever and a second pass keeps reporting Corrected even
    // though the bytes have stopped changing.
    const ChecksumResult secondPass =
        ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(secondPass.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(secondPass.romData, result.romData);
}

TEST(ChecksumPortable, HitachiM32rKlineBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R K-Line ECU Checksum");
    compareChangedBytes(original, result.romData, {{0x7fffa, bytes::Bytes{0x5a, 0xa5}}});

    const ChecksumResult secondPass =
        ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(secondPass.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(secondPass.message, "Subaru Hitachi M32R K-Line ECU Checksum");
    compareChangedBytes(result.romData, secondPass.romData, {{0x8100, bytes::Bytes{0xff, 0xff}}});

    const ChecksumResult unchangedResult =
        ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::ByteView(secondPass.romData));
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, secondPass.romData);
}

TEST(ChecksumPortable, HitachiSh7058BalancesZeroRom)
{
    const bytes::Bytes original(0x100000, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Hitachi SH7058 CAN ECU Checksum");
    compareChangedBytes(original, result.romData,
                        {{0xffff0, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}},
                         {0xffff4, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}},
                         {0xffff8, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}}});

    const ChecksumResult unchangedResult =
        ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, result.romData);
}

TEST(ChecksumPortable, HitachiSh72543rBalancesZeroRom)
{
    const bytes::Bytes original(0x200000, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Hitachi SH72543r ECU Checksum");
    compareChangedBytes(original, result.romData, {{0x1ffffe, bytes::Bytes{0x5a, 0xa5}}});

    const ChecksumResult unchangedResult =
        ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, result.romData);
}

TEST(ChecksumPortable, MitsuMh8104TcuBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result = ChecksumTcuMitsuMH8104Can::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
    compareChangedBytes(original, result.romData, {{0x81fc, bytes::Bytes{0x5a, 0xa5, 0x5a, 0xa5}}});

    const ChecksumResult unchangedResult =
        ChecksumTcuMitsuMH8104Can::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, result.romData);
}

TEST(ChecksumPortable, DensoSh7055TcuBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result = ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Denso SH7055 TCU Checksum");
    compareChangedBytes(original, result.romData, {{0x7fff4, bytes::Bytes{0x5a, 0xa5}}});

    const ChecksumResult unchangedResult =
        ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, result.romData);
}

TEST(ChecksumPortable, HitachiM32rCanTcuBalancesZeroRom)
{
    const bytes::Bytes original(0x10000, 0x00);

    const ChecksumResult result = ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
    compareChangedBytes(original, result.romData,
                        {{0x8000, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8004, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8020, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}}});

    const ChecksumResult unchangedResult =
        ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::ByteView(result.romData));
    EXPECT_EQ(unchangedResult.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(unchangedResult.romData, result.romData);
}
