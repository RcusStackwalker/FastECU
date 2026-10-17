#include "src/algorithms/checksum/denso_checksum_table.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "src/algorithms/memory/address.h"

namespace internal = fastecu::checksum::internal;
namespace memory = fastecu::memory;

namespace
{
constexpr std::uint32_t kTarget = 0x5AA5A55A;
constexpr std::size_t kTablePosition = 0x100;

// Bytes from ECU 0 whose words below the table hold 1, 2, 3, ...; one record
// for the block from `start` through `end` sits at kTablePosition.
bytes::Bytes RomWithRecord(std::uint32_t start, std::uint32_t end, std::uint32_t stored = 0)
{
    bytes::Bytes rom(kTablePosition + 12, 0);
    for (std::uint32_t word = 0; word < kTablePosition / 4; ++word)
    {
        bytes::WriteU32Be(rom, std::size_t{word} * 4, word + 1);
    }
    bytes::WriteU32Be(rom, kTablePosition, start);
    bytes::WriteU32Be(rom, kTablePosition + 4, end);
    bytes::WriteU32Be(rom, kTablePosition + 8, stored);
    return rom;
}

internal::DensoTableOutcome CorrectOneRecord(bytes::Bytes& rom)
{
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{kTablePosition}, .table_length = 12};
    return internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec);
}

// A record's block as the ECU's own record check sees it.
struct BlockCase
{
    std::string_view name; // shows up in the gtest test name
    std::uint32_t start;
    std::uint32_t end;
    bool accepted;
    std::uint32_t sum; // of the words RomWithRecord puts there, when accepted
};

std::string BlockCaseName(const ::testing::TestParamInfo<BlockCase>& info)
{
    return std::string(info.param.name);
}

constexpr std::array<BlockCase, 13> kBlockCases{{
    {"EmptyAtZero", 0, 0, true, 0},
    {"EmptyAtAWord", 8, 8, true, 0},
    // Zero is an ordinary address.
    {"FirstWord", 0, 3, true, 1},
    {"OneWord", 4, 7, true, 2},
    {"TwoWords", 4, 11, true, 2 + 3},
    {"LastWordBelowTheTable", 0xFC, 0xFF, true, 64},
    {"UnalignedStart", 5, 11, false, 0},
    {"EmptyAtAnUnalignedStart", 6, 6, false, 0},
    {"EndOnAWordsFirstByte", 4, 8, false, 0},
    {"EndInsideAWord", 4, 10, false, 0},
    {"EndBeforeStart", 8, 7, false, 0},
    {"ZeroEnd", 8, 0, false, 0},
    {"EndOneWordBeforeStart", 12, 11, false, 0},
}};

class DensoChecksumBlock : public ::testing::TestWithParam<BlockCase>
{
};

TEST_P(DensoChecksumBlock, IsSummedOnlyWhenTheEcuAcceptsIt)
{
    const BlockCase& c = GetParam();
    bytes::Bytes rom = RomWithRecord(c.start, c.end);
    const bytes::Bytes original = rom;

    const internal::DensoTableOutcome outcome = CorrectOneRecord(rom);

    if (c.accepted)
    {
        EXPECT_EQ(outcome, internal::DensoTableOutcome::kCorrected);
        EXPECT_EQ(bytes::ReadU32Be(rom, kTablePosition + 8), kTarget - c.sum);
    }
    else
    {
        EXPECT_EQ(outcome, internal::DensoTableOutcome::kMalformedBlockRange);
        EXPECT_EQ(rom, original);
    }
}

INSTANTIATE_TEST_SUITE_P(DensoChecksumTable, DensoChecksumBlock, ::testing::ValuesIn(kBlockCases), BlockCaseName);
} // namespace

// The ECU sums every record; one holding the target passes only if its words
// sum to zero.
TEST(DensoChecksumTable, ARecordHoldingTheTargetIsStillSummed)
{
    bytes::Bytes rom = RomWithRecord(4, 7, kTarget);

    EXPECT_EQ(CorrectOneRecord(rom), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, kTablePosition + 8), kTarget - 2);
}

TEST(DensoChecksumTable, AMatchingRecordIsUnchanged)
{
    bytes::Bytes rom = RomWithRecord(4, 7, kTarget - 2);
    const bytes::Bytes original = rom;

    EXPECT_EQ(CorrectOneRecord(rom), internal::DensoTableOutcome::kUnchanged);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, ABlockMayEndOnTheLastByteOfTheBytes)
{
    bytes::Bytes rom(16, 0);
    bytes::WriteU32Be(rom, 0, 12);
    bytes::WriteU32Be(rom, 4, 15);
    bytes::WriteU32Be(rom, 12, 1);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 8), kTarget - 1);
}

TEST(DensoChecksumTable, ABlockPastTheEndOfTheBytesIsAnInvalidBlockRange)
{
    bytes::Bytes rom(16, 0);
    bytes::WriteU32Be(rom, 0, 12);
    bytes::WriteU32Be(rom, 4, 19);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec),
              internal::DensoTableOutcome::kInvalidBlockRange);
    EXPECT_EQ(rom, original);
}

// The bytes start at ECU 0x1000: the block [0x1000, 0x1003] is rom[0, 4).
TEST(DensoChecksumTable, ABlockMayStartAtTheBase)
{
    bytes::Bytes rom(16, 0);
    bytes::WriteU32Be(rom, 0, 1);
    bytes::WriteU32Be(rom, 4, 0x1000);
    bytes::WriteU32Be(rom, 8, 0x1003);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0x1004}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0x1000}, rom, spec),
              internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 12), kTarget - 1);
}

TEST(DensoChecksumTable, ABlockStartingBelowTheBaseIsAnInvalidBlockRange)
{
    bytes::Bytes rom(16, 0);
    bytes::WriteU32Be(rom, 4, 0x0FFC);
    bytes::WriteU32Be(rom, 8, 0x1003);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0x1004}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0x1000}, rom, spec),
              internal::DensoTableOutcome::kInvalidBlockRange);
    EXPECT_EQ(rom, original);
}

// An empty block reads nothing, so it may name any address.
TEST(DensoChecksumTable, AnEmptyBlockBelowTheBaseSumsNothing)
{
    bytes::Bytes rom(12, 0);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0x1000}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0x1000}, rom, spec),
              internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 8), kTarget);
}

// Legacy dropped the address offset after an all-zero record; records stay
// ECU addresses throughout now.
TEST(DensoChecksumTable, RecordsAfterAnEmptyRecordAreStillEcuAddresses)
{
    bytes::Bytes rom(28, 0);
    bytes::WriteU32Be(rom, 0, 1);       // ECU 0x1000
    bytes::WriteU32Be(rom, 16, 0x1000); // record 1: ECU [0x1000, 0x1003]
    bytes::WriteU32Be(rom, 20, 0x1003);
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{0x1004}, // rom[4]: records at rom[4], rom[16]
        .table_length = 24,
        .detect_disabled = false,
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0x1000}, rom, spec),
              internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 12), kTarget);
    EXPECT_EQ(bytes::ReadU32Be(rom, 24), kTarget - 1);
}

// The block [0, 7] has two words; the override names the second.
TEST(DensoChecksumTable, AnOverrideReplacesItsWord)
{
    bytes::Bytes rom = RomWithRecord(0, 7);
    const internal::DensoWordOverride override{4, 10};
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{kTablePosition},
        .table_length = 12,
        .overrides = std::span<const internal::DensoWordOverride>(&override, 1),
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, kTablePosition + 8), kTarget - (1 + 10));
}

TEST(DensoChecksumTable, AnOverrideJustPastTheBlockIsNotSummed)
{
    bytes::Bytes rom = RomWithRecord(0, 3);
    const internal::DensoWordOverride override{4, 10};
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{kTablePosition},
        .table_length = 12,
        .overrides = std::span<const internal::DensoWordOverride>(&override, 1),
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, kTablePosition + 8), kTarget - 1);
}

TEST(DensoChecksumTable, CorrectsEveryRecordOfTheTable)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 0, 1);
    bytes::WriteU32Be(rom, 4, 2);
    bytes::WriteU32Be(rom, 8, 0); // record 0: [0, 3]
    bytes::WriteU32Be(rom, 12, 3);
    bytes::WriteU32Be(rom, 20, 0); // record 1: [0, 7]
    bytes::WriteU32Be(rom, 24, 7);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{8}, .table_length = 24};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 16), kTarget - 1);
    EXPECT_EQ(bytes::ReadU32Be(rom, 28), kTarget - (1 + 2));
}

// A later record the ECU rejects leaves the earlier, correctable one alone.
TEST(DensoChecksumTable, ARejectedRecordLeavesTheWholeTableAlone)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 0, 1);
    bytes::WriteU32Be(rom, 8, 0); // record 0: [0, 3]
    bytes::WriteU32Be(rom, 12, 3);
    bytes::WriteU32Be(rom, 20, 8); // record 1: [8, 7]
    bytes::WriteU32Be(rom, 24, 7);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{8}, .table_length = 24};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec),
              internal::DensoTableOutcome::kMalformedBlockRange);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, RejectsInvalidRecordLengthWithoutMutation)
{
    bytes::Bytes rom(16, 0x11);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 10};
    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec),
              internal::DensoTableOutcome::kInvalidRecordLength);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, RejectsATableOutsideTheBytes)
{
    bytes::Bytes rom(24, 0);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{4}, rom, spec),
              internal::DensoTableOutcome::kInvalidTableRange);
}

// An empty table fits anywhere in the bytes, even right at their end.
TEST(DensoChecksumTable, AnEmptyTableAtTheEndOfTheBytesIsUnchanged)
{
    bytes::Bytes rom(24, 0);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{24}, .table_length = 0};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kUnchanged);
}

TEST(DensoChecksumTable, DetectDisabledFalseTreatsMarkerAsOrdinaryRecord)
{
    bytes::Bytes rom(12, 0);
    bytes::WriteU32Be(rom, 8, kTarget);
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{0},
        .table_length = 12,
        .detect_disabled = false,
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kUnchanged);
}
