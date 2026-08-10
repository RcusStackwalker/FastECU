#include "src/algorithms/checksum/checksum_primitives.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <thread>

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

TEST(Checksum8, SumsPayloadBytes)
{
    const bytes::Bytes frame{0x80, 0x10, 0xF0, 0x01, 0xBF};
    EXPECT_EQ(fastecu::checksum::checksum8(bytes::ByteView(frame)),
              static_cast<bytes::Byte>(0x80 + 0x10 + 0xF0 + 0x01 + 0xBF));
}

TEST(Checksum8, ComplementsAgainst0x100WhenRequested)
{
    const bytes::Bytes payload{0xA8, 0x00, 0x11, 0x22, 0x33};
    EXPECT_EQ(fastecu::checksum::checksum8(bytes::ByteView(payload), false), bytes::Byte(0x0E));
    EXPECT_EQ(fastecu::checksum::checksum8(bytes::ByteView(payload), true), bytes::Byte(0xF2));
}

TEST(Crc32, MatchesKnownPolynomialVector)
{
    const bytes::Bytes payload{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_EQ(fastecu::checksum::crc32(bytes::ByteView(payload)), std::uint32_t(0x8FA3DEB3));
}

TEST(Crc32, PointerOverloadMatchesByteViewOverload)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(fastecu::checksum::crc32(payload.data(), std::uint32_t(payload.size())),
              std::uint32_t(0x8AD85CF9));
}

TEST(Crc32, NullPointerReturnsZero)
{
    EXPECT_EQ(fastecu::checksum::crc32(nullptr, 8), std::uint32_t(0));
}

TEST(Crc32, FirstTouchOfLazyTableIsThreadSafe)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    auto fn = [&payload]()
    {
        return fastecu::checksum::crc32(payload.data(), std::uint32_t(payload.size()));
    };

    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::thread ta([&]()
                   { a = fn(); });
    std::thread tb([&]()
                   { b = fn(); });
    ta.join();
    tb.join();
    EXPECT_EQ(a, std::uint32_t(0x8AD85CF9));
    EXPECT_EQ(b, std::uint32_t(0x8AD85CF9));
}
