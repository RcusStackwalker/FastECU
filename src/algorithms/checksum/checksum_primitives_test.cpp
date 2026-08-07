#include "src/algorithms/checksum/checksum_primitives.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

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

TEST(CksAdd8, ReturnsZeroForEmptyData)
{
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>{}), std::uint8_t(0));
}

TEST(CksAdd8, SumsBytesWithoutCarry)
{
    const std::array<std::uint8_t, 3> data{0x01, 0x02, 0x03};
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>(data)), std::uint8_t(6));
}

TEST(CksAdd8, AddsOneOnCarry)
{
    // Plain mod-256 truncation of 0xFF + 0xFF would give 0xFE; the
    // "add 1 on carry" step this checksum is named for makes it 0xFF.
    const std::array<std::uint8_t, 2> data{0xFF, 0xFF};
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>(data)), std::uint8_t(0xFF));
}

TEST(CksAdd8, MatchesReflashBlockShape)
{
    // Matches EcuOperations::npk_raw_flashblock's real call shape: a
    // 131-byte block (3-byte address header + 128-byte payload).
    // Repeated carry corrections over 131 additions of 0x02 give 7,
    // not the naive mod-256 sum of 131*2 = 262 -> 6.
    std::array<std::uint8_t, 131> data{};
    data.fill(0x02);
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>(data)), std::uint8_t(7));
}
