#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.h"
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

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/testing/memory_views.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Portable-type mirror of tests/test_checksum_results.cpp -- same input and
// expected-output byte vectors for all nine checksum families (the frozen
// Qt contract), expressed with bytes::Bytes/bytes::ByteView instead of
// QByteArray/QString, exercised again here so this package's own test
// target proves the portable API round-trips (math included) without
// depending on //tests or linking Qt.

namespace memory = fastecu::memory;

// A fixed layout's size is its parameter type: bytes of any other length do
// not convert to it implicitly.
template <typename Family>
constexpr bool kTakesOnlyItsLayout =
    std::is_invocable_v<decltype(&Family::CalculateChecksumResult), typename Family::Rom> &&
    !std::is_invocable_v<decltype(&Family::CalculateChecksumResult), bytes::ByteView>;
static_assert(kTakesOnlyItsLayout<ChecksumEcuSubaruHitachiM32rCan>);
static_assert(kTakesOnlyItsLayout<ChecksumEcuSubaruHitachiM32rKline>);
static_assert(kTakesOnlyItsLayout<ChecksumEcuSubaruHitachiSH7058>);
static_assert(kTakesOnlyItsLayout<ChecksumEcuSubaruHitachiSh72543r>);
static_assert(kTakesOnlyItsLayout<ChecksumTcuMitsuMH8104Can>);
static_assert(kTakesOnlyItsLayout<ChecksumTcuSubaruDensoSH7055>);
namespace
{

void CompareChangedBytes(const bytes::Bytes& original, const bytes::Bytes& actual,
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
bytes::Bytes DensoRomWithChecksumTable(std::uint32_t stored_checksum)
{
    bytes::Bytes rom(4, 0x00);
    bytes::AppendU32Be(rom, 0x00000001);
    rom.resize(rom.size() + 8, 0x00);
    bytes::AppendU32Be(rom, 0x00000004);
    bytes::AppendU32Be(rom, 0x00000008);
    bytes::AppendU32Be(rom, stored_checksum);
    return rom;
}

} // namespace

TEST(ChecksumPortable, DensoSh705xDieselCorrectsSingleZeroRecord)
{
    const bytes::Bytes original(12, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(original)), memory::FlashAddress{0}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Denso SH705x Checksum");
    CompareChangedBytes(original, result.rom_data,
                        {{0, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa5, 0xa5, 0x5a}}});
}

TEST(ChecksumPortable, DensoSh705xDieselCorrectedRecordTriggersDisabledCompatibility)
{
    const bytes::Bytes original = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa5, 0xa5, 0x5a};

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(original)), memory::FlashAddress{0}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kDisabled);
    EXPECT_EQ(result.message, "ROM has all checksums disabled");
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}

TEST(ChecksumPortable, DensoSh705xDieselKeepsMatchingRecord)
{
    const bytes::Bytes original = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x5a, 0xa5, 0xa5, 0x52};

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(original)), memory::FlashAddress{0}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}

TEST(ChecksumPortable, DensoSh7xxxReturnsUnchangedForMatchingChecksum)
{
    const bytes::Bytes rom = DensoRomWithChecksumTable(0x5aa5a559);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(rom)), memory::FlashAddress{16}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(rom));
    EXPECT_TRUE(result.Ok());
    EXPECT_FALSE(result.Changed());
}

TEST(ChecksumPortable, DensoSh7xxxReturnsCorrectedDataForMismatchedChecksum)
{
    const bytes::Bytes rom = DensoRomWithChecksumTable(0x00000000);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(rom)), memory::FlashAddress{16}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    bytes::Bytes expected = rom;
    const bytes::Bytes corrected_checksum = {0x5a, 0xa5, 0xa5, 0x59};
    std::copy(corrected_checksum.begin(), corrected_checksum.end(), expected.begin() + 24);
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(expected));
    EXPECT_TRUE(result.Ok());
    EXPECT_TRUE(result.Changed());

    const ChecksumResult unchanged_result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(result.rom_data)), memory::FlashAddress{16}, 12);
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(result.rom_data));
}

TEST(ChecksumPortable, DensoSh7xxxReturnsDisabledWithoutClearingRomData)
{
    bytes::Bytes rom(16, 0x00);
    bytes::AppendU32Be(rom, 0x00000000);
    bytes::AppendU32Be(rom, 0x00000000);
    bytes::AppendU32Be(rom, 0x5aa5a55a);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(rom)), memory::FlashAddress{16}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kDisabled);
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(rom));
    EXPECT_TRUE(result.Ok());
}

TEST(ChecksumPortable, DensoSh7xxxRejectsChecksumAreaOutsideRom)
{
    const bytes::Bytes rom(16, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(rom)), memory::FlashAddress{16}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kInvalidSize);
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(rom));
    EXPECT_FALSE(result.Ok());
}

TEST(ChecksumPortable, DensoSh7xxxRejectsNonTableAlignedChecksumArea)
{
    const bytes::Bytes rom(16, 0x00);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(bytes::ByteView(rom)), memory::FlashAddress{0}, 10);

    EXPECT_EQ(result.status, ChecksumResult::Status::kParseError);
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(rom));
    EXPECT_FALSE(result.Ok());
}

TEST(ChecksumPortable, HitachiM32rCanBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(ChecksumEcuSubaruHitachiM32rCan::Rom(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R CAN ECU Checksum");
    CompareChangedBytes(original, result.rom_data, {{0x7fffa, bytes::Bytes{0x5a, 0xa5}}});

    // Checksum 6's mismatch handling never writes the fix back to romData
    // (dead code in the algorithm, unchanged by this task), so checksum_ok
    // stays false forever and a second pass keeps reporting Corrected even
    // though the bytes have stopped changing.
    const ChecksumResult second_pass =
        ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(ChecksumEcuSubaruHitachiM32rCan::Rom(result.rom_data));
    EXPECT_EQ(second_pass.status, ChecksumResult::Status::kCorrected);
    EXPECT_THAT(second_pass.rom_data, test_bytes::BytesEq(result.rom_data));
}

TEST(ChecksumPortable, HitachiM32rKlineBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruHitachiM32rKline::CalculateChecksumResult(ChecksumEcuSubaruHitachiM32rKline::Rom(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R K-Line ECU Checksum");
    CompareChangedBytes(original, result.rom_data, {{0x7fffa, bytes::Bytes{0x5a, 0xa5}}});

    const ChecksumResult second_pass = ChecksumEcuSubaruHitachiM32rKline::CalculateChecksumResult(
        ChecksumEcuSubaruHitachiM32rKline::Rom(result.rom_data));
    EXPECT_EQ(second_pass.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(second_pass.message, "Subaru Hitachi M32R K-Line ECU Checksum");
    CompareChangedBytes(result.rom_data, second_pass.rom_data, {{0x8100, bytes::Bytes{0xff, 0xff}}});

    const ChecksumResult unchanged_result = ChecksumEcuSubaruHitachiM32rKline::CalculateChecksumResult(
        ChecksumEcuSubaruHitachiM32rKline::Rom(second_pass.rom_data));
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(second_pass.rom_data));
}

TEST(ChecksumPortable, HitachiSh7058BalancesZeroRom)
{
    const bytes::Bytes original(0x100000, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruHitachiSH7058::CalculateChecksumResult(ChecksumEcuSubaruHitachiSH7058::Rom(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Hitachi SH7058 CAN ECU Checksum");
    CompareChangedBytes(original, result.rom_data,
                        {{0xffff0, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}},
                         {0xffff4, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}},
                         {0xffff8, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}}});

    const ChecksumResult unchanged_result =
        ChecksumEcuSubaruHitachiSH7058::CalculateChecksumResult(ChecksumEcuSubaruHitachiSH7058::Rom(result.rom_data));
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(result.rom_data));
}

TEST(ChecksumPortable, HitachiSh72543rBalancesZeroRom)
{
    const bytes::Bytes original(0x200000, 0x00);

    const ChecksumResult result =
        ChecksumEcuSubaruHitachiSh72543r::CalculateChecksumResult(ChecksumEcuSubaruHitachiSh72543r::Rom(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Hitachi SH72543r ECU Checksum");
    CompareChangedBytes(original, result.rom_data, {{0x1ffffe, bytes::Bytes{0x5a, 0xa5}}});

    const ChecksumResult unchanged_result = ChecksumEcuSubaruHitachiSh72543r::CalculateChecksumResult(
        ChecksumEcuSubaruHitachiSh72543r::Rom(result.rom_data));
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(result.rom_data));
}

TEST(ChecksumPortable, MitsuMh8104TcuBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result =
        ChecksumTcuMitsuMH8104Can::CalculateChecksumResult(ChecksumTcuMitsuMH8104Can::Rom(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
    CompareChangedBytes(original, result.rom_data, {{0x81fc, bytes::Bytes{0x5a, 0xa5, 0x5a, 0xa5}}});

    const ChecksumResult unchanged_result =
        ChecksumTcuMitsuMH8104Can::CalculateChecksumResult(ChecksumTcuMitsuMH8104Can::Rom(result.rom_data));
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(result.rom_data));
}

TEST(ChecksumPortable, DensoSh7055TcuBalancesZeroRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result =
        ChecksumTcuSubaruDensoSH7055::CalculateChecksumResult(ChecksumTcuSubaruDensoSH7055::Rom(original));
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Denso SH7055 TCU Checksum");
    CompareChangedBytes(original, result.rom_data, {{0x7fff4, bytes::Bytes{0x5a, 0xa5}}});

    const ChecksumResult unchanged_result =
        ChecksumTcuSubaruDensoSH7055::CalculateChecksumResult(ChecksumTcuSubaruDensoSH7055::Rom(result.rom_data));
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(result.rom_data));
}

// The family sums the whole file: a 512 KiB M32R_512KB read corrects the same
// fixed fields as a smaller file (legacy checksum_tcu_subaru_hitachi_m32r_can).
TEST(ChecksumPortable, HitachiM32rCanTcuBalancesA512KibRom)
{
    const bytes::Bytes original(0x80000, 0x00);

    const ChecksumResult result = ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(original);
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    CompareChangedBytes(original, result.rom_data,
                        {{0x8000, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8004, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8020, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}}});

    const ChecksumResult unchanged_result = ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(result.rom_data);
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
}

// The checksum fields end at 0x8024 and the sums read whole 32-bit words.
TEST(ChecksumPortable, HitachiM32rCanTcuRejectsAFileTooShortOrNotWordAligned)
{
    for (const std::size_t size : {std::size_t{0x8020}, std::size_t{0x80001}})
    {
        const bytes::Bytes original(size, 0x3C);
        const ChecksumResult result = ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(original);
        EXPECT_EQ(result.status, ChecksumResult::Status::kInvalidSize) << size;
        EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
        EXPECT_EQ(result.message, "ROM size does not match the checksum layout");
    }
}

// 0x8024 is the shortest file that holds both fields, so it is corrected like
// any larger one: checksum 1 is then its balance word alone.
TEST(ChecksumPortable, HitachiM32rCanTcuBalancesTheShortestFileThatHoldsItsFields)
{
    const bytes::Bytes original(0x8024, 0x00);

    const ChecksumResult result = ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(original);
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    CompareChangedBytes(original, result.rom_data,
                        {{0x8000, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8004, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8020, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}}});
}

TEST(ChecksumPortable, HitachiM32rCanTcuBalancesZeroRom)
{
    const bytes::Bytes original(0x10000, 0x00);

    const ChecksumResult result = ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(original);
    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(result.message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
    CompareChangedBytes(original, result.rom_data,
                        {{0x8000, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8004, bytes::Bytes{0xa5, 0x5a, 0x5a, 0xa6}},
                         {0x8020, bytes::Bytes{0x5a, 0xa5, 0xa5, 0x5a}}});

    const ChecksumResult unchanged_result = ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(result.rom_data);
    EXPECT_EQ(unchanged_result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_THAT(unchanged_result.rom_data, test_bytes::BytesEq(result.rom_data));
}

TEST(ChecksumPortable, DensoDieselRejectsMalformedTableWithoutMutation)
{
    const bytes::Bytes original(24, 0x22);
    const ChecksumResult malformed = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0}, 10);
    EXPECT_EQ(malformed.status, ChecksumResult::Status::kParseError);
    EXPECT_EQ(malformed.message, "Checksum area length must be a multiple of 12 bytes");
    EXPECT_THAT(malformed.rom_data, test_bytes::BytesEq(original));

    const ChecksumResult out_of_range = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{20}, 12);
    EXPECT_EQ(out_of_range.status, ChecksumResult::Status::kInvalidSize);
    EXPECT_EQ(out_of_range.message, "ROM is too small for the configured checksum area");
    EXPECT_THAT(out_of_range.rom_data, test_bytes::BytesEq(original));
}

TEST(ChecksumPortable, DensoDieselReportsAPrimaryBlockPastTheEndOfTheRom)
{
    bytes::Bytes original(24, 0);
    bytes::WriteU32Be(original, 0, 4);
    bytes::WriteU32Be(original, 4, 28);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kInvalidSize);
    EXPECT_EQ(result.message, "ROM is too small for a checksum block range");
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}

// Two table addresses each have one word, 0x84 bytes below the table, that the
// sum counts as 0xFFFFFFFF whatever the ROM holds there.
struct DensoDieselFixedWordCase
{
    std::string_view name; // shows up in the gtest test name
    std::uint32_t table_address;
    std::uint32_t fixed_word_address;
};

std::string DensoDieselFixedWordCaseName(const ::testing::TestParamInfo<DensoDieselFixedWordCase>& info)
{
    return std::string(info.param.name);
}

constexpr std::array<DensoDieselFixedWordCase, 2> kDensoDieselFixedWordCases{{
    {"Table0FFB80", 0x0FFB80, 0x0FFAFC},
    {"Table17FB80", 0x17FB80, 0x17FAFC},
}};

class DensoDieselFixedWord : public ::testing::TestWithParam<DensoDieselFixedWordCase>
{
};

// The bytes start at the fixed word, which holds 0x12345678; the table's one
// record sums just that word.
TEST_P(DensoDieselFixedWord, SumsTheWordAsAllOnes)
{
    const DensoDieselFixedWordCase& c = GetParam();
    const std::size_t table_position = c.table_address - c.fixed_word_address;
    bytes::Bytes original(table_position + 12, 0);
    bytes::WriteU32Be(original, 0, 0x12345678);
    bytes::WriteU32Be(original, table_position, c.fixed_word_address);
    bytes::WriteU32Be(original, table_position + 4, c.fixed_word_address + 4);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original, memory::FlashAddress{c.fixed_word_address}),
        memory::FlashAddress{c.table_address}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(result.rom_data, table_position + 8), 0x5AA5A55AU - 0xFFFFFFFFU);
}

INSTANTIATE_TEST_SUITE_P(ChecksumPortable, DensoDieselFixedWord, ::testing::ValuesIn(kDensoDieselFixedWordCases),
                         DensoDieselFixedWordCaseName);

TEST(ChecksumPortable, DensoDieselSecondaryFailureRollsBackPrimaryCorrection)
{
    bytes::Bytes original(0x200000, 0);
    bytes::WriteU32Be(original, 0x1FF8E8, 0x1FFFFC);
    bytes::WriteU32Be(original, 0x1FF8EC, 0x200004);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0x1FF800}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kInvalidSize);
    EXPECT_EQ(result.message, "ROM is too small for a checksum block range");
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}

TEST(ChecksumPortable, DensoDieselCorrectsSh72543SecondaryTableAfterPrimary)
{
    bytes::Bytes original(0x200000, 0);
    bytes::WriteU32Be(original, 4, 1);
    bytes::WriteU32Be(original, 0x1FF800, 4);
    bytes::WriteU32Be(original, 0x1FF804, 8);
    bytes::WriteU32Be(original, 0x1FF808, 0x5AA5A559);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0x1FF800}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(result.rom_data, 0x1FF8F0), 0x5AA5A55AU);
    EXPECT_EQ(bytes::ReadU32Be(result.rom_data, 0x1FF8FC), 0x5AA5A55AU);
}

TEST(ChecksumPortable, DensoDieselReversedSecondaryBlockRollsBackPrimaryCorrection)
{
    bytes::Bytes original(0x200000, 0);
    bytes::WriteU32Be(original, 0x1FF8E8, 0x1FFF00);
    bytes::WriteU32Be(original, 0x1FF8EC, 0x1FFE00);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0x1FF800}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kParseError);
    EXPECT_EQ(result.message, "A checksum block range ends before it starts");
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}

TEST(ChecksumPortable, DensoSh7xxxReportsAReversedBlockRange)
{
    bytes::Bytes original(24, 0);
    bytes::WriteU32Be(original, 0, 20);
    bytes::WriteU32Be(original, 4, 8);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kParseError);
    EXPECT_EQ(result.message, "A checksum block range ends before it starts");
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}

TEST(ChecksumPortable, DensoSh7xxxReportsInvalidBlockRangeMessage)
{
    bytes::Bytes original(24, 0);
    bytes::WriteU32Be(original, 0, 20);
    bytes::WriteU32Be(original, 4, 28);

    const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        memory::testing::ViewOf(original), memory::FlashAddress{0}, 12);

    EXPECT_EQ(result.status, ChecksumResult::Status::kInvalidSize);
    EXPECT_EQ(result.message, "ROM is too small for a checksum block range");
    EXPECT_THAT(result.rom_data, test_bytes::BytesEq(original));
}
