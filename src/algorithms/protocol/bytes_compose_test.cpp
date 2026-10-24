#include "src/algorithms/protocol/bytes_compose.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <type_traits>

using ::testing::ElementsAre;

using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::ComposeBeWithExtraCapacity;
using bytes::U24;
using namespace bytes::literals;

// Rejected at compile time by the width law; kept as documentation because
// gtest cannot assert a static_assert failure:
//   composeBe(0x34);                      // int
//   composeBe('K');                       // char
//   composeBe(std::size_t{4});            // size_t
//   composeBe(0x1FF_b);                   // literal too wide for a byte
//   ComposeBe();                         // statically empty output
//   ComposeBe(std::array<bytes::Byte, 0>{}); // statically empty output

TEST(ComposeBe, ComposesScalarsIntoAConstexprArray)
{
    constexpr auto frame = ComposeBe(0x12_b, std::uint16_t{0x3456}, U24{0xFF789ABC}, std::uint32_t{0xDEF01234});
    static_assert(std::same_as<std::remove_cv_t<decltype(frame)>, std::array<bytes::Byte, 10>>);
    static_assert(frame == std::array<bytes::Byte, 10>{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34});
    EXPECT_THAT(frame, ElementsAre(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34));
}

TEST(ComposeBe, ComposesFixedRangesIntoAConstexprArray)
{
    constexpr std::array<bytes::Byte, 2> prefix{0x12, 0x34};
    constexpr std::uint16_t words[]{0x5678, 0x9ABC};
    constexpr auto frame = ComposeBe(prefix, std::span<const std::uint16_t, 2>{words});
    static_assert(std::same_as<std::remove_cv_t<decltype(frame)>, std::array<bytes::Byte, 6>>);
    static_assert(frame == std::array<bytes::Byte, 6>{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC});
    EXPECT_THAT(ComposeBe(words), ElementsAre(0x56, 0x78, 0x9A, 0xBC));
}

TEST(ComposeBe, UsesRuntimeValuesFromMutableFixedRanges)
{
    bytes::Byte prefix[]{0x12};
    std::array<std::uint32_t, 1> word{0x3456789A};
    auto frame = ComposeBe(prefix, std::span{word});
    static_assert(std::same_as<decltype(frame), std::array<bytes::Byte, 5>>);
    EXPECT_THAT(frame, ElementsAre(0x12, 0x34, 0x56, 0x78, 0x9A));
}

TEST(ComposeBe, AllowsEmptyStaticRangesInNonemptyComposition)
{
    constexpr auto frame = ComposeBe(std::array<bytes::Byte, 0>{}, 0x12_b, std::span<const std::uint16_t, 0>{});
    static_assert(frame == std::array<bytes::Byte, 1>{0x12});
    EXPECT_THAT(frame, ElementsAre(0x12));
}

TEST(ComposeBe, SelectsVectorForMixedStaticAndEmptyDynamicInputs)
{
    auto frame = ComposeBe(0x12_b, bytes::ByteView{});
    static_assert(std::same_as<decltype(frame), bytes::Bytes>);
    EXPECT_THAT(frame, ElementsAre(0x12));
}

TEST(ComposeBe, ConsumesUnsizedInputRangesOnce)
{
    std::istringstream input{"ABC"};
    const auto range =
        std::ranges::subrange(std::istream_iterator<bytes::Byte>{input}, std::istream_iterator<bytes::Byte>{});
    EXPECT_THAT(ComposeBe(0x12_b, range), ElementsAre(0x12, 0x41, 0x42, 0x43));
}

TEST(ComposeBe, EmitsOneByteForByte)
{
    EXPECT_THAT(ComposeBe(0x34_b), ElementsAre(0x34));
}

TEST(ComposeBe, EmitsTwoBytesForUint16MostSignificantFirst)
{
    EXPECT_THAT(ComposeBe(std::uint16_t{0xBEEF}), ElementsAre(0xBE, 0xEF));
}

TEST(ComposeBe, EmitsThreeBytesForU24MostSignificantFirst)
{
    EXPECT_THAT(ComposeBe(U24{0x123456}), ElementsAre(0x12, 0x34, 0x56));
}

TEST(ComposeBe, TruncatesU24ToItsLowThreeBytes)
{
    EXPECT_THAT(ComposeBe(U24{0xFF123456}), ElementsAre(0x12, 0x34, 0x56));
}

TEST(ComposeBe, EmitsFourBytesForUint32MostSignificantFirst)
{
    EXPECT_THAT(ComposeBe(std::uint32_t{0x12345678}), ElementsAre(0x12, 0x34, 0x56, 0x78));
}

TEST(ComposeBe, SplicesByteRangesInline)
{
    const bytes::Bytes payload{0xAA, 0xBB};
    const std::array<bytes::Byte, 2> tail{0xCC, 0xDD};
    EXPECT_THAT(ComposeBe(0x01_b, bytes::ByteView(payload), tail), ElementsAre(0x01, 0xAA, 0xBB, 0xCC, 0xDD));
}

TEST(ComposeBe, SplicesWordRangesAsFourBytesEachMostSignificantFirst)
{
    const std::array<std::uint32_t, 2> words{0x01020304, 0xA0B0C0D0};
    EXPECT_THAT(ComposeBe(0xEE_b, words), ElementsAre(0xEE, 0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0));
}

TEST(ComposeBe, SplicesHalfwordRangesAsTwoBytesEachMostSignificantFirst)
{
    const std::array<std::uint16_t, 2> halfwords{0x0102, 0xA0B0};
    EXPECT_THAT(ComposeBe(halfwords), ElementsAre(0x01, 0x02, 0xA0, 0xB0));
}

TEST(ComposeBe, SplicesDoublewordRangesAsEightBytesEachMostSignificantFirst)
{
    const std::array<std::uint64_t, 1> doublewords{0x0102030405060708};
    EXPECT_THAT(ComposeBe(doublewords), ElementsAre(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08));
}

TEST(ComposeBeWithExtraCapacity, ReservesTheWidthOfEveryElementOfASizedIntegerRange)
{
    const std::array<std::uint32_t, 3> words{1, 2, 3};
    EXPECT_GE(ComposeBeWithExtraCapacity(0, words).capacity(), 12U);
}

TEST(ComposeBe, EmitsNothingForAnEmptyWordRange)
{
    auto frame = ComposeBe(std::span<const std::uint32_t>{});
    static_assert(std::same_as<decltype(frame), bytes::Bytes>);
    EXPECT_THAT(frame, ElementsAre());
}

TEST(ComposeBe, AppendsStringViewCharsAsBytes)
{
    EXPECT_THAT(ComposeBe(std::string_view{"KERN2"}), ElementsAre(0x4B, 0x45, 0x52, 0x4E, 0x32));
}

TEST(ComposeBe, EmitsNothingForAnEmptyDynamicByteRange)
{
    EXPECT_THAT(ComposeBe(bytes::ByteView{}), ElementsAre());
}

TEST(ComposeBe, AppendsArgumentsLeftToRight)
{
    EXPECT_THAT(ComposeBe(0x34_b, U24{0x00A000}, 0x04_b, U24{0x000200}),
                ElementsAre(0x34, 0x00, 0xA0, 0x00, 0x04, 0x00, 0x02, 0x00));
}

TEST(ComposeBeWithExtraCapacity, ReservesWithoutEmitting)
{
    const bytes::Bytes out = ComposeBeWithExtraCapacity(4, 0x01_b, 0x02_b);
    EXPECT_THAT(out, ElementsAre(0x01, 0x02));
    EXPECT_GE(out.capacity(), out.size() + 4);
}

TEST(ComposeBeWithExtraCapacity, AlwaysReturnsAVectorIncludingEmptyOutput)
{
    auto frame = ComposeBeWithExtraCapacity(0, 0x12_b);
    static_assert(std::same_as<decltype(frame), bytes::Bytes>);
    EXPECT_THAT(frame, ElementsAre(0x12));
    EXPECT_THAT(ComposeBeWithExtraCapacity(0), ElementsAre());
}

TEST(ComposeBeWithChecksum, ComposesConstexprArrayAndChecksumsOnlyThePayload)
{
    constexpr auto frame = ComposeBeWithChecksum(
        [](bytes::ByteView data) { return std::uint16_t(data.size() + data[0]); }, 0x12_b, std::uint16_t{0x3456});
    static_assert(frame == std::array<bytes::Byte, 5>{0x12, 0x34, 0x56, 0, 0x15});
    EXPECT_THAT(frame, ElementsAre(0x12, 0x34, 0x56, 0, 0x15));
}

TEST(ComposeBeWithChecksum, AllowsChecksumOnlyComposition)
{
    constexpr auto frame = ComposeBeWithChecksum([](bytes::ByteView data) { return bytes::Byte(0x80 + data.size()); });
    static_assert(frame == std::array<bytes::Byte, 1>{0x80});
    EXPECT_THAT(frame, ElementsAre(0x80));
}

TEST(ComposeBeWithChecksum, KeepsDynamicEmptyPayloadInAVector)
{
    auto frame = ComposeBeWithChecksum([](bytes::ByteView data) { return std::uint32_t(0x12345678 + data.size()); },
                                       bytes::ByteView{});
    static_assert(std::same_as<decltype(frame), bytes::Bytes>);
    EXPECT_THAT(frame, ElementsAre(0x12, 0x34, 0x56, 0x78));
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
    EXPECT_THAT(ComposeBeWithChecksum(sum, 0x80_b, 0x10_b, 0xF0_b, 0x01_b), ElementsAre(0x80, 0x10, 0xF0, 0x01, 0x81));
}

TEST(ComposeBeWithChecksum, AppendsFourBytesForUint32ReturningFunction)
{
    const auto fixed = [](bytes::ByteView) { return std::uint32_t{0x5AA5A55A}; };
    EXPECT_THAT(ComposeBeWithChecksum(fixed, 0x01_b), ElementsAre(0x01, 0x5A, 0xA5, 0xA5, 0x5A));
}

TEST(ComposeBeWithChecksum, ComputesChecksumOverEverythingComposed)
{
    const auto count = [](bytes::ByteView data) { return bytes::Byte(data.size()); };
    const bytes::Bytes payload{0xAA, 0xBB, 0xCC};
    EXPECT_THAT(ComposeBeWithChecksum(count, std::uint16_t{0xBEEF}, bytes::ByteView(payload)),
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
    const bytes::Bytes framed =
        ComposeBeWithChecksum(sum, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), bytes::ByteView(payload));
    EXPECT_THAT(framed, ElementsAre(0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3));
}
