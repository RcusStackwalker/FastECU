#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/protocol/bytes.h"

#include <limits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::ElementsAre;

TEST(BytesPortable, ReadsBigEndianWidths)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::ReadU16Be(view), 0x1234U);
    EXPECT_EQ(bytes::ReadU24Be(view), 0x123456U);
    EXPECT_EQ(bytes::ReadU32Be(view), 0x12345678U);
}

TEST(BytesPortable, ReadsLittleEndianWidths)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::ReadU16Le(view), 0x3412U);
    EXPECT_EQ(bytes::ReadU24Le(view), 0x563412U);
    EXPECT_EQ(bytes::ReadU32Le(view), 0x78563412U);
}

TEST(BytesPortable, AppendsBigEndianWidths)
{
    bytes::Bytes out;
    bytes::AppendU16Be(out, 0x1234);
    bytes::AppendU24Be(out, 0x563412);
    EXPECT_THAT(out, ElementsAre(0x12, 0x34, 0x56, 0x34, 0x12));
}

TEST(BytesPortable, Sums8OverFullView)
{
    const bytes::Bytes buf{0x01, 0x02, 0x03, 0xFF};
    EXPECT_EQ(bytes::Sum8(bytes::ByteView(buf)), static_cast<bytes::Byte>(0x05));
}

TEST(BytesPortable, ToHexRendersLowercasePairsWithTrailingSpace)
{
    const bytes::Bytes buf{0x80, 0x01, 0x02, 0xFF};
    EXPECT_EQ(bytes::ToHex(bytes::ByteView(buf)), "80 01 02 ff ");
    EXPECT_EQ(bytes::ToHex(bytes::ByteView()), "");
    EXPECT_EQ(bytes::ToHex(bytes::ByteView(buf), "{:02X}"), "800102FF");
    EXPECT_EQ(bytes::ToHex(bytes::ByteView(), "{:02X}"), "");
}

TEST(BytesPortable, ReadsVariableWidthBigEndian)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 1), 0x12U);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 2), 0x1234U);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 3), 0x123456U);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 4), 0x12345678U);
    EXPECT_EQ(bytes::ReadUBe(view, 2, 2), 0x5678U);
}

TEST(BytesPortable, ReadsVariableWidthLittleEndian)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::ReadULe(view, 0, 1), 0x12U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 2), 0x3412U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 3), 0x563412U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 4), 0x78563412U);
    EXPECT_EQ(bytes::ReadULe(view, 2, 2), 0x7856U);
}

TEST(BytesPortable, VariableWidthReadsReturnZeroOutOfRange)
{
    const bytes::Bytes buf{0x12, 0x34};
    const bytes::ByteView view(buf);
    // Window runs past the end at every width.
    EXPECT_EQ(bytes::ReadUBe(view, 2, 1), 0U);
    EXPECT_EQ(bytes::ReadUBe(view, 1, 2), 0U);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 3), 0U);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 4), 0U);
    EXPECT_EQ(bytes::ReadULe(view, 2, 1), 0U);
    EXPECT_EQ(bytes::ReadULe(view, 1, 2), 0U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 3), 0U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 4), 0U);
}

TEST(BytesPortable, VariableWidthReadsRejectOutOfDomainWidths)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 0), 0U);
    EXPECT_EQ(bytes::ReadUBe(view, 0, 5), 0U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 0), 0U);
    EXPECT_EQ(bytes::ReadULe(view, 0, 5), 0U);
}

TEST(BytesPortable, VariableWidthReadsRejectMaximumOffset)
{
    const bytes::Bytes buf{0x12};
    const bytes::ByteView view(buf);
    constexpr std::size_t kMaximumOffset = std::numeric_limits<std::size_t>::max();

    EXPECT_EQ(bytes::ReadUBe(view, kMaximumOffset, 2), 0U);
    EXPECT_EQ(bytes::ReadULe(view, kMaximumOffset, 2), 0U);
}

TEST(BytesPortable, OverwriteAtPreservesTruncatingContract)
{
    const bytes::Bytes payload{0xAA, 0xBB, 0xCC};

    bytes::Bytes exact{0, 1, 2, 3, 4};
    bytes::OverwriteAt(exact, 2, payload);
    EXPECT_THAT(exact, ElementsAre(0, 1, 0xAA, 0xBB, 0xCC));

    bytes::Bytes truncated{0, 1, 2, 3};
    bytes::OverwriteAt(truncated, 2, payload);
    EXPECT_THAT(truncated, ElementsAre(0, 1, 0xAA, 0xBB));

    bytes::Bytes empty_payload{0, 1};
    bytes::OverwriteAt(empty_payload, 1, bytes::ByteView{});
    EXPECT_THAT(empty_payload, ElementsAre(0, 1));
}

TEST(BytesPortable, OverwriteAtDoesNothingAtOrBeyondEnd)
{
    constexpr std::size_t kMaximumOffset = std::numeric_limits<std::size_t>::max();
    const bytes::Bytes payload{0xAA};
    bytes::Bytes at_end{0, 1};
    bytes::Bytes beyond_end = at_end;
    bytes::Bytes maximum_offset = at_end;

    bytes::OverwriteAt(at_end, at_end.size(), payload);
    bytes::OverwriteAt(beyond_end, beyond_end.size() + 1, payload);
    bytes::OverwriteAt(maximum_offset, kMaximumOffset, payload);

    EXPECT_THAT(at_end, ElementsAre(0, 1));
    EXPECT_THAT(beyond_end, ElementsAre(0, 1));
    EXPECT_THAT(maximum_offset, ElementsAre(0, 1));
}

TEST(BytesPortable, FixedWidthWritersRejectShortAndMaximumOffsets)
{
    constexpr std::size_t kMaximumOffset = std::numeric_limits<std::size_t>::max();
    bytes::Bytes short_buffer{0x11, 0x22, 0x33};
    const bytes::Bytes original = short_buffer;

    bytes::WriteU16Be(short_buffer, kMaximumOffset, 0xAABB);
    bytes::WriteU24Be(short_buffer, kMaximumOffset, 0xAABBCC);
    bytes::WriteU32Be(short_buffer, kMaximumOffset, 0xAABBCCDD);
    bytes::WriteU24Be(short_buffer, 1, 0xAABBCC);
    bytes::WriteU32Be(short_buffer, 0, 0xAABBCCDD);
    bytes::WriteU16Le(short_buffer, kMaximumOffset, 0xAABB);
    bytes::WriteU24Le(short_buffer, kMaximumOffset, 0xAABBCC);
    bytes::WriteU32Le(short_buffer, kMaximumOffset, 0xAABBCCDD);
    bytes::WriteU24Le(short_buffer, 1, 0xAABBCC);
    bytes::WriteU32Le(short_buffer, 0, 0xAABBCCDD);

    EXPECT_EQ(short_buffer, original);
}

TEST(BytesPortable, FixedWidthWritersAcceptExactBoundary)
{
    bytes::Bytes out(18, 0);
    bytes::WriteU16Be(out, 0, 0x1234);
    bytes::WriteU24Be(out, 2, 0x56789A);
    bytes::WriteU32Be(out, 5, 0xBCDEF012);
    bytes::WriteU16Le(out, 9, 0x3456);
    bytes::WriteU24Le(out, 11, 0x789ABC);
    bytes::WriteU32Le(out, 14, 0xDEF01234);

    EXPECT_THAT(out, ElementsAre(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x56, 0x34, 0xBC, 0x9A, 0x78,
                                 0x34, 0x12, 0xF0, 0xDE));
}

TEST(BytesPortable, sum8Range_sumsOnlyTheRequestedWindow)
{
    const bytes::Bytes buf{0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(bytes::Sum8Range(bytes::ByteView(buf), 1, 2), static_cast<bytes::Byte>(0x05));
}

TEST(BytesPortable, sum8Range_clampsLengthToWhatIsAvailable)
{
    const bytes::Bytes buf{0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(bytes::Sum8Range(bytes::ByteView(buf), 2, 99), static_cast<bytes::Byte>(0x07));
}

TEST(BytesPortable, sum8_isPassableAsACallable)
{
    bytes::Byte (*fn)(bytes::ByteView) = bytes::Sum8;
    const bytes::Bytes buf{0x01, 0x02};
    EXPECT_EQ(fn(bytes::ByteView(buf)), static_cast<bytes::Byte>(0x03));
}
