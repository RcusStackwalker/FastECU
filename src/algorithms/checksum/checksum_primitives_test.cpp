#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/checksum/checksum_primitives.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

using ::testing::ElementsAre;

TEST(ChecksumPrimitives, RebalancesU16BigEndianFromStoredValue)
{
    bytes::Bytes rom{0x12, 0x34};
    fastecu::checksum::internal::RebalanceU16Be(rom, 0, 0xFF00, 0x0010);
    EXPECT_THAT(rom, ElementsAre(0x13, 0x44));
}

TEST(ChecksumPrimitives, RebalancesU16WithModularWraparound)
{
    bytes::Bytes rom{0xFF, 0xF0};
    fastecu::checksum::internal::RebalanceU16Be(rom, 0, 0x0000, 0x0020);
    EXPECT_THAT(rom, ElementsAre(0x00, 0x10));
}

TEST(ChecksumPrimitives, RebalancesU32BigEndianWithModularWraparound)
{
    bytes::Bytes rom{0xFF, 0xFF, 0xFF, 0xF0};
    fastecu::checksum::internal::RebalanceU32Be(rom, 0, 0x00000000, 0x00000020);
    EXPECT_THAT(rom, ElementsAre(0x00, 0x00, 0x00, 0x10));
}

TEST(ChecksumPrimitives, RebalancesU16DownwardAtNonzeroOffset)
{
    bytes::Bytes rom{0xA5, 0x12, 0x34, 0x5A};
    fastecu::checksum::internal::RebalanceU16Be(rom, 1, 0x0020, 0x0010);
    EXPECT_THAT(rom, ElementsAre(0xA5, 0x12, 0x24, 0x5A));
}

TEST(ChecksumPrimitives, RebalancesU32DownwardAtNonzeroOffset)
{
    bytes::Bytes rom{0xA5, 0x12, 0x34, 0x56, 0x78, 0x5A};
    fastecu::checksum::internal::RebalanceU32Be(rom, 1, 0x00000020, 0x00000010);
    EXPECT_THAT(rom, ElementsAre(0xA5, 0x12, 0x34, 0x56, 0x68, 0x5A));
}

TEST(ChecksumPrimitives, ChecksumsOnlyTheRequestedSubview)
{
    const bytes::Bytes payload{0xA5, 0x80, 0x3C};
    const auto subview = bytes::ByteView(payload).subspan(1, 1);
    EXPECT_EQ(fastecu::checksum::Crc32(subview), std::uint32_t(0xE876B489));
    EXPECT_EQ(fastecu::checksum::CksAdd8(subview), bytes::Byte(0x80));
    EXPECT_EQ(fastecu::checksum::NegatedSum8(subview), bytes::Byte(0x80));
}

TEST(CksAdd8, ReturnsZeroForEmptyData)
{
    EXPECT_EQ(fastecu::checksum::CksAdd8(std::span<const std::uint8_t>{}), std::uint8_t(0));
}

TEST(CksAdd8, SumsBytesWithoutCarry)
{
    const std::array<std::uint8_t, 3> data{0x01, 0x02, 0x03};
    EXPECT_EQ(fastecu::checksum::CksAdd8(std::span<const std::uint8_t>(data)), std::uint8_t(6));
}

TEST(CksAdd8, AddsOneOnCarry)
{
    // Plain mod-256 truncation of 0xFF + 0xFF would give 0xFE; the
    // "add 1 on carry" step this checksum is named for makes it 0xFF.
    const std::array<std::uint8_t, 2> data{0xFF, 0xFF};
    EXPECT_EQ(fastecu::checksum::CksAdd8(std::span<const std::uint8_t>(data)), std::uint8_t(0xFF));
}

TEST(CksAdd8, PreservesASingleHighBitByte)
{
    const std::array<std::uint8_t, 1> data{0x80};
    EXPECT_EQ(fastecu::checksum::CksAdd8(data), std::uint8_t(0x80));
}

TEST(CksAdd8, ContinuesAddingAfterCarryCorrection)
{
    const std::array<std::uint8_t, 3> data{0xFF, 0x01, 0x01};
    EXPECT_EQ(fastecu::checksum::CksAdd8(data), std::uint8_t(0x02));
}

TEST(CksAdd8, SumsMixedHighAndLowBytes)
{
    const std::array<std::uint8_t, 3> data{0x80, 0x7F, 0x80};
    EXPECT_EQ(fastecu::checksum::CksAdd8(data), std::uint8_t(0x80));
}

TEST(CksAdd8, MatchesReflashBlockShape)
{
    // Matches the call shape of the deleted EcuOperations::npk_raw_flashblock: a
    // 131-byte block (3-byte address header + 128-byte payload).
    // Repeated carry corrections over 131 additions of 0x02 give 7,
    // not the naive mod-256 sum of 131*2 = 262 -> 6.
    std::array<std::uint8_t, 131> data{};
    data.fill(0x02);
    EXPECT_EQ(fastecu::checksum::CksAdd8(std::span<const std::uint8_t>(data)), std::uint8_t(7));
}

TEST(NegatedSum8, ComplementsTheSumAgainst0x100)
{
    const bytes::Bytes payload{0xA8, 0x00, 0x11, 0x22, 0x33};
    EXPECT_EQ(fastecu::checksum::NegatedSum8(bytes::ByteView(payload)), bytes::Byte(0xF2));
}

TEST(NegatedSum8, YieldsZeroForAZeroSum)
{
    const bytes::Bytes payload{0x00, 0x00};
    EXPECT_EQ(fastecu::checksum::NegatedSum8(bytes::ByteView(payload)), bytes::Byte(0x00));
}

TEST(NegatedSum8, ReturnsZeroForEmptyData)
{
    EXPECT_EQ(fastecu::checksum::NegatedSum8(bytes::ByteView{}), bytes::Byte(0x00));
}

TEST(NegatedSum8, ReturnsZeroWhenTheSumWrapsToZero)
{
    const bytes::Bytes payload{0xFF, 0x01};
    EXPECT_EQ(fastecu::checksum::NegatedSum8(payload), bytes::Byte(0x00));
}

TEST(NegatedSum8, NegatesANonzeroWrappedSum)
{
    const bytes::Bytes payload{0xFF, 0x02};
    EXPECT_EQ(fastecu::checksum::NegatedSum8(payload), bytes::Byte(0xFF));
}

TEST(NegatedSum8, IsThePlainSumSubtractedFrom0x100)
{
    const bytes::Bytes frame{0x80, 0x10, 0xF0, 0x01, 0xBF};
    const bytes::Byte plain = bytes::Sum8(bytes::ByteView(frame));
    EXPECT_EQ(fastecu::checksum::NegatedSum8(bytes::ByteView(frame)), static_cast<bytes::Byte>(0x100 - plain));
}

TEST(Crc32, MatchesKnownPolynomialVector)
{
    const bytes::Bytes payload{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_EQ(fastecu::checksum::Crc32(bytes::ByteView(payload)), std::uint32_t(0x8FA3DEB3));
}

// These literals were independently calculated without the production table:
// start at 0xFFFFFFFF, XOR in each byte, perform eight reflected bit shifts
// with polynomial 0x5AA5A55A, then XOR the result with 0xFFFFFFFF.
TEST(Crc32, ReturnsZeroForEmptyData)
{
    EXPECT_EQ(fastecu::checksum::Crc32(bytes::ByteView{}), std::uint32_t(0x00000000));
}

TEST(Crc32, MatchesSingleZeroByteVector)
{
    const bytes::Bytes payload{0x00};
    EXPECT_EQ(fastecu::checksum::Crc32(payload), std::uint32_t(0xB2D311D3));
}

TEST(Crc32, MatchesSingleHighBitByteVector)
{
    const bytes::Bytes payload{0x80};
    EXPECT_EQ(fastecu::checksum::Crc32(payload), std::uint32_t(0xE876B489));
}

TEST(Crc32, MatchesAlternatingByteVector)
{
    const bytes::Bytes payload{0xAA, 0x55, 0xAA, 0x55};
    EXPECT_EQ(fastecu::checksum::Crc32(payload), std::uint32_t(0xA5C473EB));
}

TEST(Crc32, PointerOverloadMatchesByteViewOverload)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(fastecu::checksum::Crc32(payload.data(), std::uint32_t(payload.size())), std::uint32_t(0x8AD85CF9));
}

TEST(Crc32, NullPointerReturnsZero)
{
    EXPECT_EQ(fastecu::checksum::Crc32(nullptr, 8), std::uint32_t(0));
}

TEST(Crc32, NullPointerWithZeroLengthReturnsZero)
{
    EXPECT_EQ(fastecu::checksum::Crc32(nullptr, 0), std::uint32_t(0));
}

TEST(Crc32, NonNullPointerWithZeroLengthReturnsZero)
{
    const bytes::Bytes payload{0x80};
    EXPECT_EQ(fastecu::checksum::Crc32(payload.data(), 0), std::uint32_t(0));
}
