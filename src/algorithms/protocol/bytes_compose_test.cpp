#include "src/algorithms/protocol/bytes_compose.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

using ::testing::ElementsAre;

using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::composeBeWithExtraCapacity;
using bytes::u24;
using namespace bytes::literals;

// Rejected at compile time by the width law; kept as documentation because
// gtest cannot assert a static_assert failure:
//   composeBe(0x34);                      // int
//   composeBe('K');                       // char
//   composeBe(std::size_t{4});            // size_t
//   composeBe(0x1FF_b);                   // literal too wide for a byte

TEST(ComposeBe, EmitsOneByteForByte)
{
    EXPECT_THAT(composeBe(0x34_b), ElementsAre(0x34));
}

TEST(ComposeBe, EmitsTwoBytesForUint16MostSignificantFirst)
{
    EXPECT_THAT(composeBe(std::uint16_t{0xBEEF}), ElementsAre(0xBE, 0xEF));
}

TEST(ComposeBe, EmitsThreeBytesForU24MostSignificantFirst)
{
    EXPECT_THAT(composeBe(u24(0x123456)), ElementsAre(0x12, 0x34, 0x56));
}

TEST(ComposeBe, TruncatesU24ToItsLowThreeBytes)
{
    EXPECT_THAT(composeBe(u24(0xFF123456)), ElementsAre(0x12, 0x34, 0x56));
}

TEST(ComposeBe, EmitsFourBytesForUint32MostSignificantFirst)
{
    EXPECT_THAT(composeBe(std::uint32_t{0x12345678}), ElementsAre(0x12, 0x34, 0x56, 0x78));
}

TEST(ComposeBe, SplicesByteRangesInline)
{
    const bytes::Bytes payload{0xAA, 0xBB};
    const std::array<bytes::Byte, 2> tail{0xCC, 0xDD};
    EXPECT_THAT(composeBe(0x01_b, bytes::ByteView(payload), tail),
                ElementsAre(0x01, 0xAA, 0xBB, 0xCC, 0xDD));
}

TEST(ComposeBe, AppendsStringViewCharsAsBytes)
{
    EXPECT_THAT(composeBe(std::string_view{"KERN2"}),
                ElementsAre(0x4B, 0x45, 0x52, 0x4E, 0x32));
}

TEST(ComposeBe, EmitsNothingForNoArguments)
{
    EXPECT_EQ(composeBe(), bytes::Bytes{});
}

TEST(ComposeBe, AppendsArgumentsLeftToRight)
{
    EXPECT_THAT(composeBe(0x34_b, u24(0x00A000), 0x04_b, u24(0x000200)),
                ElementsAre(0x34, 0x00, 0xA0, 0x00, 0x04, 0x00, 0x02, 0x00));
}

TEST(ComposeBeWithExtraCapacity, ReservesWithoutEmitting)
{
    const bytes::Bytes out = composeBeWithExtraCapacity(4, 0x01_b, 0x02_b);
    EXPECT_THAT(out, ElementsAre(0x01, 0x02));
    EXPECT_GE(out.capacity(), out.size() + 4);
}

TEST(ComposeBeWithChecksum, AppendsOneByteForByteReturningFunction)
{
    const auto sum = [](bytes::ByteView data)
    {
        bytes::Byte total = 0;
        for (const bytes::Byte value : data)
        {
            total = static_cast<bytes::Byte>(total + value);
        }
        return total;
    };
    // 0x80 + 0x10 = 0x90; + 0xF0 = 0x180 -> 0x80; + 0x01 = 0x81.
    EXPECT_THAT(composeBeWithChecksum(sum, 0x80_b, 0x10_b, 0xF0_b, 0x01_b),
                ElementsAre(0x80, 0x10, 0xF0, 0x01, 0x81));
}

TEST(ComposeBeWithChecksum, AppendsFourBytesForUint32ReturningFunction)
{
    const auto fixed = [](bytes::ByteView)
    { return std::uint32_t{0x5AA5A55A}; };
    EXPECT_THAT(composeBeWithChecksum(fixed, 0x01_b),
                ElementsAre(0x01, 0x5A, 0xA5, 0xA5, 0x5A));
}

TEST(ComposeBeWithChecksum, ComputesChecksumOverEverythingComposed)
{
    const auto count = [](bytes::ByteView data)
    { return bytes::Byte(data.size()); };
    const bytes::Bytes payload{0xAA, 0xBB, 0xCC};
    EXPECT_THAT(composeBeWithChecksum(count, std::uint16_t{0xBEEF}, bytes::ByteView(payload)),
                ElementsAre(0xBE, 0xEF, 0xAA, 0xBB, 0xCC, 0x05));
}

// The SSM header shape, asserted against literal hex rather than recomputed.
TEST(ComposeBeWithChecksum, BuildsTheSsmHeaderFrame)
{
    const auto sum = [](bytes::ByteView data)
    {
        bytes::Byte total = 0;
        for (const bytes::Byte value : data)
        {
            total = static_cast<bytes::Byte>(total + value);
        }
        return total;
    };
    const bytes::Bytes payload{0xEF, 0x52};
    const bytes::Bytes framed = composeBeWithChecksum(
        sum, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), bytes::ByteView(payload));
    EXPECT_THAT(framed, ElementsAre(0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3));
}
