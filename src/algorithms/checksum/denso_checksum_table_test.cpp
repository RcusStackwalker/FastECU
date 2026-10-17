#include "src/algorithms/checksum/denso_checksum_table.h"

#include <gtest/gtest.h>

#include <span>

#include "src/algorithms/memory/address.h"

namespace internal = fastecu::checksum::internal;
namespace memory = fastecu::memory;

TEST(DensoChecksumTable, CorrectsCompleteTableAtomically)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 4, 1);
    bytes::WriteU32Be(rom, 8, 0);
    bytes::WriteU32Be(rom, 12, 4);
    bytes::WriteU32Be(rom, 20, 4);
    bytes::WriteU32Be(rom, 24, 8);

    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{8}, .table_length = 24};
    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 16), 0x5AA5A55AU);
    EXPECT_EQ(bytes::ReadU32Be(rom, 28), 0x5AA5A559U);
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

TEST(DensoChecksumTable, RejectsInvalidBlockWithoutPartialMutation)
{
    bytes::Bytes rom(24, 0);
    bytes::WriteU32Be(rom, 0, 4);
    bytes::WriteU32Be(rom, 4, 8);
    bytes::WriteU32Be(rom, 12, 20);
    bytes::WriteU32Be(rom, 16, 28);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 24};
    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec),
              internal::DensoTableOutcome::kInvalidBlockRange);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, DescendingRangeKeepsLegacyEmptySumBehavior)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 20, 12);
    bytes::WriteU32Be(rom, 24, 4);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{20}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 28), 0x5AA5A55AU);
}

// With a nonzero base, a start address below it wraps above the end: a
// descending range that sums nothing, so the record is "corrected" to the
// target, as legacy does for any descending range.
TEST(DensoChecksumTable, AStartBelowTheBaseSumsNothing)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 16, 0x0800);
    bytes::WriteU32Be(rom, 20, 0x1008);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0x1010}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0x1000}, rom, spec),
              internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 24), 0x5AA5A55AU);
}

// An end address below a nonzero base wraps past the end of the bytes.
TEST(DensoChecksumTable, AnEndBelowTheBaseIsAnInvalidBlockRange)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 16, 0x1004);
    bytes::WriteU32Be(rom, 20, 0x0800);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0x1010}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0x1000}, rom, spec),
              internal::DensoTableOutcome::kInvalidBlockRange);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, UnalignedRangeKeepsLegacyWordTraversalBehavior)
{
    bytes::Bytes rom(40, 0);
    bytes::WriteU32Be(rom, 4, 1);
    bytes::WriteU32Be(rom, 8, 2);
    bytes::WriteU32Be(rom, 24, 4);
    bytes::WriteU32Be(rom, 28, 10);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{24}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 32), 0x5AA5A557U);
}

TEST(DensoChecksumTable, DetectDisabledFalseTreatsMarkerAsOrdinaryRecord)
{
    bytes::Bytes rom(12, 0);
    bytes::WriteU32Be(rom, 8, 0x5AA5A55A);
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{0},
        .table_length = 12,
        .detect_disabled = false,
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kUnchanged);
}

// The bytes start at ECU 4: the record's ECU range [8, 12) is rom[4, 8), and
// the override names ECU address 8.
TEST(DensoChecksumTable, IndexesRecordsByEcuAddressFromTheBase)
{
    bytes::Bytes rom(32, 0);
    bytes::WriteU32Be(rom, 4, 1);
    bytes::WriteU32Be(rom, 20, 8);
    bytes::WriteU32Be(rom, 24, 12);
    const internal::DensoWordOverride override{8, 0xFFFFFFFF};
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{24},
        .table_length = 12,
        .overrides = std::span<const internal::DensoWordOverride>(&override, 1),
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{4}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 28), 0x5AA5A55BU);
}

// Legacy dropped the address offset after an all-zero record; records stay
// ECU addresses throughout now.
TEST(DensoChecksumTable, RecordsAfterAnEmptyRecordAreStillEcuAddresses)
{
    bytes::Bytes rom(48, 0);
    bytes::WriteU32Be(rom, 4, 1);  // ECU 8
    bytes::WriteU32Be(rom, 8, 2);  // ECU 12
    bytes::WriteU32Be(rom, 36, 8); // record 2: ECU [8, 12)
    bytes::WriteU32Be(rom, 40, 12);
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{16}, // rom[12]: records at rom[12], rom[24], rom[36]
        .table_length = 36,
        .detect_disabled = false,
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{4}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 44), 0x5AA5A55AU - 1);
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

TEST(DensoChecksumTable, ABlockMayEndAtTheEndOfTheBytes)
{
    bytes::Bytes rom(16, 0);
    bytes::WriteU32Be(rom, 0, 12);
    bytes::WriteU32Be(rom, 4, 16);
    bytes::WriteU32Be(rom, 12, 1);
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 8), 0x5AA5A559U);
}

// The block [12, 18) ends inside the bytes, but its second word, [16, 20),
// runs past them.
TEST(DensoChecksumTable, AWordRunningPastTheEndOfTheBytesIsAnInvalidBlockRange)
{
    bytes::Bytes rom(18, 0);
    bytes::WriteU32Be(rom, 0, 12);
    bytes::WriteU32Be(rom, 4, 18);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_address = memory::FlashAddress{0}, .table_length = 12};

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{0}, rom, spec),
              internal::DensoTableOutcome::kInvalidBlockRange);
    EXPECT_EQ(rom, original);
}

// The block [4, 12) is rom[0, 8); the override names its second word.
TEST(DensoChecksumTable, AnOverrideReplacesALaterWordOfTheBlock)
{
    bytes::Bytes rom(20, 0);
    bytes::WriteU32Be(rom, 4, 1);
    bytes::WriteU32Be(rom, 8, 4);
    bytes::WriteU32Be(rom, 12, 12);
    const internal::DensoWordOverride override{8, 3};
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{12},
        .table_length = 12,
        .overrides = std::span<const internal::DensoWordOverride>(&override, 1),
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{4}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 16), 0x5AA5A55AU - 3);
}

// The block [4, 8) is rom[0, 4); it stops before the overridden word at 8.
TEST(DensoChecksumTable, AnOverrideJustPastTheBlockIsNotSummed)
{
    bytes::Bytes rom(20, 0);
    bytes::WriteU32Be(rom, 8, 4);
    bytes::WriteU32Be(rom, 12, 8);
    const internal::DensoWordOverride override{8, 1};
    const internal::DensoTableSpec spec{
        .table_address = memory::FlashAddress{12},
        .table_length = 12,
        .overrides = std::span<const internal::DensoWordOverride>(&override, 1),
    };

    EXPECT_EQ(internal::CorrectDensoTable(memory::FlashAddress{4}, rom, spec), internal::DensoTableOutcome::kCorrected);
    EXPECT_EQ(bytes::ReadU32Be(rom, 16), 0x5AA5A55AU);
}
