#include "src/algorithms/checksum/denso_checksum_table.h"

#include <gtest/gtest.h>

namespace internal = fastecu::checksum::internal;

TEST(DensoChecksumTable, CorrectsCompleteTableAtomically)
{
    bytes::Bytes rom(32, 0);
    bytes::writeU32Be(rom, 4, 1);
    bytes::writeU32Be(rom, 8, 0);
    bytes::writeU32Be(rom, 12, 4);
    bytes::writeU32Be(rom, 20, 4);
    bytes::writeU32Be(rom, 24, 8);

    const internal::DensoTableSpec spec{.table_offset = 8, .table_length = 24};
    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::Corrected);
    EXPECT_EQ(bytes::readU32Be(rom, 16), 0x5AA5A55Au);
    EXPECT_EQ(bytes::readU32Be(rom, 28), 0x5AA5A559u);
}

TEST(DensoChecksumTable, RejectsInvalidRecordLengthWithoutMutation)
{
    bytes::Bytes rom(16, 0x11);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_offset = 0, .table_length = 10};
    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::InvalidRecordLength);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, RejectsInvalidBlockWithoutPartialMutation)
{
    bytes::Bytes rom(24, 0);
    bytes::writeU32Be(rom, 0, 4);
    bytes::writeU32Be(rom, 4, 8);
    bytes::writeU32Be(rom, 12, 20);
    bytes::writeU32Be(rom, 16, 28);
    const bytes::Bytes original = rom;
    const internal::DensoTableSpec spec{.table_offset = 0, .table_length = 24};
    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::InvalidBlockRange);
    EXPECT_EQ(rom, original);
}

TEST(DensoChecksumTable, AppliesNegativeOffsetAndWordOverride)
{
    bytes::Bytes rom(32, 0);
    bytes::writeU32Be(rom, 4, 1);
    bytes::writeU32Be(rom, 20, 8);
    bytes::writeU32Be(rom, 24, 12);
    const internal::DensoWordOverride override{4, 0xFFFFFFFF};
    const internal::DensoTableSpec spec{
        .table_offset = 20,
        .table_length = 12,
        .address_offset = -4,
        .overrides = std::span<const internal::DensoWordOverride>(&override, 1),
    };

    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::Corrected);
    EXPECT_EQ(bytes::readU32Be(rom, 28), 0x5AA5A55Bu);
}

TEST(DensoChecksumTable, ZeroAddressRecordResetsAddressOffset)
{
    bytes::Bytes rom(36, 0);
    bytes::writeU32Be(rom, 12, 8);
    bytes::writeU32Be(rom, 16, 12);
    bytes::writeU32Be(rom, 20, 0x5AA5A55A);
    bytes::writeU32Be(rom, 32, 0);
    const internal::DensoTableSpec spec{
        .table_offset = 12,
        .table_length = 24,
        .address_offset = -4,
        .detect_disabled = false,
    };

    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::Corrected);
    EXPECT_EQ(bytes::readU32Be(rom, 32), 0x5AA5A55Au);
}

TEST(DensoChecksumTable, DescendingRangeKeepsLegacyEmptySumBehavior)
{
    bytes::Bytes rom(32, 0);
    bytes::writeU32Be(rom, 20, 12);
    bytes::writeU32Be(rom, 24, 4);
    const internal::DensoTableSpec spec{.table_offset = 20, .table_length = 12};

    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::Corrected);
    EXPECT_EQ(bytes::readU32Be(rom, 28), 0x5AA5A55Au);
}

TEST(DensoChecksumTable, UnalignedRangeKeepsLegacyWordTraversalBehavior)
{
    bytes::Bytes rom(40, 0);
    bytes::writeU32Be(rom, 4, 1);
    bytes::writeU32Be(rom, 8, 2);
    bytes::writeU32Be(rom, 24, 4);
    bytes::writeU32Be(rom, 28, 10);
    const internal::DensoTableSpec spec{.table_offset = 24, .table_length = 12};

    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::Corrected);
    EXPECT_EQ(bytes::readU32Be(rom, 32), 0x5AA5A557u);
}

TEST(DensoChecksumTable, DetectDisabledFalseTreatsMarkerAsOrdinaryRecord)
{
    bytes::Bytes rom(12, 0);
    bytes::writeU32Be(rom, 8, 0x5AA5A55A);
    const internal::DensoTableSpec spec{
        .table_offset = 0,
        .table_length = 12,
        .detect_disabled = false,
    };

    EXPECT_EQ(internal::correctDensoTable(rom, spec), internal::DensoTableOutcome::Unchanged);
}
