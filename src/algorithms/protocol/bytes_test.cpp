#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::ElementsAre;

namespace
{

// The view under test is the front of a larger buffer whose tail holds guard
// bytes. An accessor that strays past the view's end reads a guard byte or
// overwrites one, so the test sees it rather than reading or writing whatever
// lies past a vector's allocation.
constexpr std::size_t kViewSize = 8;
constexpr bytes::Byte kGuard = 0xEE;
constexpr std::size_t kGuardSize = 4;

bytes::Bytes GuardedBuffer()
{
    bytes::Bytes buffer(kViewSize, 0x00);
    buffer.insert(buffer.end(), kGuardSize, kGuard);
    return buffer;
}

// One fixed-width reader/writer pair. `encoded` is `value` as it appears on
// the wire, written out here rather than derived from the code under test.
struct FixedWidthAccessorCase
{
    std::string_view name; // shows up in the gtest test name
    std::size_t width;
    std::uint32_t (*read)(bytes::ByteView, std::size_t);
    void (*write)(bytes::MutableByteView, std::size_t, std::uint32_t);
    std::uint32_t value;
    bytes::Bytes encoded;
};

std::string CaseName(const ::testing::TestParamInfo<FixedWidthAccessorCase>& info)
{
    return std::string(info.param.name);
}

// The readers return and the writers take their own width; the table stores
// every pair as one function-pointer type.
template <auto Read> std::uint32_t ReadAs(bytes::ByteView view, std::size_t offset)
{
    return Read(view, offset);
}

template <auto Write, typename Value = std::uint32_t>
void WriteAs(bytes::MutableByteView view, std::size_t offset, std::uint32_t value)
{
    Write(view, offset, static_cast<Value>(value));
}

const std::array<FixedWidthAccessorCase, 6> kFixedWidthAccessors{{
    {"U16Be", 2, &ReadAs<&bytes::ReadU16Be>, &WriteAs<&bytes::WriteU16Be, std::uint16_t>, 0x1234, {0x12, 0x34}},
    {"U24Be", 3, &ReadAs<&bytes::ReadU24Be>, &WriteAs<&bytes::WriteU24Be>, 0x123456, {0x12, 0x34, 0x56}},
    {"U32Be", 4, &ReadAs<&bytes::ReadU32Be>, &WriteAs<&bytes::WriteU32Be>, 0x12345678, {0x12, 0x34, 0x56, 0x78}},
    {"U16Le", 2, &ReadAs<&bytes::ReadU16Le>, &WriteAs<&bytes::WriteU16Le, std::uint16_t>, 0x3412, {0x12, 0x34}},
    {"U24Le", 3, &ReadAs<&bytes::ReadU24Le>, &WriteAs<&bytes::WriteU24Le>, 0x563412, {0x12, 0x34, 0x56}},
    {"U32Le", 4, &ReadAs<&bytes::ReadU32Le>, &WriteAs<&bytes::WriteU32Le>, 0x78563412, {0x12, 0x34, 0x56, 0x78}},
}};

class FixedWidthAccessor : public ::testing::TestWithParam<FixedWidthAccessorCase>
{
};

// The last whole value starts at a non-zero offset for every width, so a
// bound computed as `size + offset` instead of `size - offset` is visible.
TEST_P(FixedWidthAccessor, ReadsTheLastWholeValue)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();
    std::ranges::copy(c.encoded, buffer.begin() + std::ptrdiff_t(kViewSize - c.width));

    EXPECT_EQ(c.read(bytes::ByteView(buffer).first(kViewSize), kViewSize - c.width), c.value);
}

TEST_P(FixedWidthAccessor, ReadsAValueThatFillsTheView)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();
    std::ranges::copy(c.encoded, buffer.begin());

    EXPECT_EQ(c.read(bytes::ByteView(buffer).first(c.width), 0), c.value);
}

// The bytes past the view hold the rest of the value, so a reader that
// strays past the view's end reads it.
TEST_P(FixedWidthAccessor, ReadsZeroFromAViewShorterThanTheValue)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();
    std::ranges::copy(c.encoded, buffer.begin());

    EXPECT_EQ(c.read(bytes::ByteView(buffer).first(c.width - 1), 0), 0U);
}

TEST_P(FixedWidthAccessor, ReadsZeroOnePastTheLastWholeValue)
{
    const FixedWidthAccessorCase& c = GetParam();
    const bytes::Bytes buffer = GuardedBuffer();

    EXPECT_EQ(c.read(bytes::ByteView(buffer).first(kViewSize), kViewSize - c.width + 1), 0U);
}

TEST_P(FixedWidthAccessor, ReadsZeroAtTheEndAndAtTheMaximumOffset)
{
    const FixedWidthAccessorCase& c = GetParam();
    const bytes::Bytes buffer = GuardedBuffer();
    const bytes::ByteView view = bytes::ByteView(buffer).first(kViewSize);

    EXPECT_EQ(c.read(view, kViewSize), 0U);
    EXPECT_EQ(c.read(view, std::numeric_limits<std::size_t>::max()), 0U);
}

TEST_P(FixedWidthAccessor, WritesTheLastWholeValue)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();
    bytes::Bytes expected = GuardedBuffer();
    std::ranges::copy(c.encoded, expected.begin() + std::ptrdiff_t(kViewSize - c.width));

    c.write(bytes::MutableByteView(buffer).first(kViewSize), kViewSize - c.width, c.value);

    EXPECT_EQ(buffer, expected);
}

TEST_P(FixedWidthAccessor, WritesAValueThatFillsTheView)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();
    bytes::Bytes expected = GuardedBuffer();
    std::ranges::copy(c.encoded, expected.begin());

    c.write(bytes::MutableByteView(buffer).first(c.width), 0, c.value);

    EXPECT_EQ(buffer, expected);
}

TEST_P(FixedWidthAccessor, LeavesTheBufferUntouchedInAViewShorterThanTheValue)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();

    c.write(bytes::MutableByteView(buffer).first(c.width - 1), 0, c.value);

    EXPECT_EQ(buffer, GuardedBuffer());
}

TEST_P(FixedWidthAccessor, LeavesTheBufferUntouchedOnePastTheLastWholeValue)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();

    c.write(bytes::MutableByteView(buffer).first(kViewSize), kViewSize - c.width + 1, c.value);

    EXPECT_EQ(buffer, GuardedBuffer());
}

TEST_P(FixedWidthAccessor, LeavesTheBufferUntouchedAtTheEndAndAtTheMaximumOffset)
{
    const FixedWidthAccessorCase& c = GetParam();
    bytes::Bytes buffer = GuardedBuffer();
    const bytes::MutableByteView view = bytes::MutableByteView(buffer).first(kViewSize);

    c.write(view, kViewSize, c.value);
    c.write(view, std::numeric_limits<std::size_t>::max(), c.value);

    EXPECT_EQ(buffer, GuardedBuffer());
}

INSTANTIATE_TEST_SUITE_P(BytesPortable, FixedWidthAccessor, ::testing::ValuesIn(kFixedWidthAccessors), CaseName);

} // namespace

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

    // The guard bytes past the view show a copy that runs past its end.
    bytes::Bytes truncated{0, 1, 2, 3, kGuard, kGuard};
    bytes::OverwriteAt(bytes::MutableByteView(truncated).first(4), 2, payload);
    EXPECT_THAT(truncated, ElementsAre(0, 1, 0xAA, 0xBB, kGuard, kGuard));

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
