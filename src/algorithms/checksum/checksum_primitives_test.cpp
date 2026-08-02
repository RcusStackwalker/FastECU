#include "src/algorithms/checksum/checksum_primitives.h"

#include <gtest/gtest.h>

TEST(ChecksumPrimitives, RebalancesU16BigEndianFromStoredValue)
{
    bytes::Bytes rom{0x12, 0x34};
    fastecu::checksum::internal::rebalanceU16Be(rom, 0, 0xFF00, 0x0010);
    EXPECT_EQ(rom, (bytes::Bytes{0x13, 0x44}));
}

TEST(ChecksumPrimitives, RebalancesU16WithModularWraparound)
{
    bytes::Bytes rom{0xFF, 0xF0};
    fastecu::checksum::internal::rebalanceU16Be(rom, 0, 0x0000, 0x0020);
    EXPECT_EQ(rom, (bytes::Bytes{0x00, 0x10}));
}

TEST(ChecksumPrimitives, RebalancesU32BigEndianWithModularWraparound)
{
    bytes::Bytes rom{0xFF, 0xFF, 0xFF, 0xF0};
    fastecu::checksum::internal::rebalanceU32Be(rom, 0, 0x00000000, 0x00000020);
    EXPECT_EQ(rom, (bytes::Bytes{0x00, 0x00, 0x00, 0x10}));
}
