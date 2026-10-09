#include <QByteArray>
#include <QString>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"

using ::testing::ElementsAre;

TEST(TestBytes, readU16Le_readsLowByteFirst)
{
    const std::array<bytes::Byte, 2> data{0x34, 0x12};
    ASSERT_EQ(bytes::ReadU16Le(bytes::ByteView(data)), std::uint16_t(0x1234));
}

TEST(TestBytes, readU24Le_readsLowByteFirst)
{
    const std::array<bytes::Byte, 3> data{0x56, 0x34, 0x12};
    ASSERT_EQ(bytes::ReadU24Le(bytes::ByteView(data)), std::uint32_t(0x123456));
}

TEST(TestBytes, readU32Le_readsLowByteFirst)
{
    const std::array<bytes::Byte, 4> data{0x78, 0x56, 0x34, 0x12};
    ASSERT_EQ(bytes::ReadU32Le(bytes::ByteView(data)), std::uint32_t(0x12345678));
}

TEST(TestBytes, readU16Le_respectsOffset)
{
    const std::array<bytes::Byte, 4> data{0xFF, 0xFF, 0x34, 0x12};
    ASSERT_EQ(bytes::ReadU16Le(bytes::ByteView(data), 2), std::uint16_t(0x1234));
}

TEST(TestBytes, readLe_outOfBoundsReturnsZero)
{
    const std::array<bytes::Byte, 1> data{0x12};
    ASSERT_EQ(bytes::ReadU16Le(bytes::ByteView(data)), std::uint16_t(0));
    ASSERT_EQ(bytes::ReadU24Le(bytes::ByteView(data)), std::uint32_t(0));
    ASSERT_EQ(bytes::ReadU32Le(bytes::ByteView(data)), std::uint32_t(0));
}

TEST(TestBytes, appendU16Le_appendsLowByteFirst)
{
    bytes::Bytes out;
    bytes::AppendU16Le(out, 0x1234);
    ASSERT_THAT(out, ElementsAre(0x34, 0x12));
}

TEST(TestBytes, appendU24Le_appendsLowByteFirst)
{
    bytes::Bytes out;
    bytes::AppendU24Le(out, 0x123456);
    ASSERT_THAT(out, ElementsAre(0x56, 0x34, 0x12));
}

TEST(TestBytes, appendU32Le_appendsLowByteFirst)
{
    bytes::Bytes out;
    bytes::AppendU32Le(out, 0x12345678);
    ASSERT_THAT(out, ElementsAre(0x78, 0x56, 0x34, 0x12));
}

TEST(TestBytes, writeU16Be_writesAtOffsetWithoutDisturbingRest)
{
    std::array<bytes::Byte, 4> data{0xAA, 0xAA, 0xAA, 0xAA};
    bytes::WriteU16Be(bytes::MutableByteView(data), 1, 0x1234);
    ASSERT_EQ(data, (std::array<bytes::Byte, 4>{0xAA, 0x12, 0x34, 0xAA}));
}

TEST(TestBytes, writeU24Be_writesBigEndianBytes)
{
    std::array<bytes::Byte, 3> data{};
    bytes::WriteU24Be(bytes::MutableByteView(data), 0, 0x123456);
    ASSERT_EQ(data, (std::array<bytes::Byte, 3>{0x12, 0x34, 0x56}));
}

TEST(TestBytes, writeU32Be_writesBigEndianBytes)
{
    std::array<bytes::Byte, 4> data{};
    bytes::WriteU32Be(bytes::MutableByteView(data), 0, 0x12345678);
    ASSERT_EQ(data, (std::array<bytes::Byte, 4>{0x12, 0x34, 0x56, 0x78}));
}

TEST(TestBytes, writeU16Le_writesLittleEndianBytes)
{
    std::array<bytes::Byte, 2> data{};
    bytes::WriteU16Le(bytes::MutableByteView(data), 0, 0x1234);
    ASSERT_EQ(data, (std::array<bytes::Byte, 2>{0x34, 0x12}));
}

TEST(TestBytes, writeU24Le_writesLittleEndianBytes)
{
    std::array<bytes::Byte, 3> data{};
    bytes::WriteU24Le(bytes::MutableByteView(data), 0, 0x123456);
    ASSERT_EQ(data, (std::array<bytes::Byte, 3>{0x56, 0x34, 0x12}));
}

TEST(TestBytes, writeU32Le_writesLittleEndianBytes)
{
    std::array<bytes::Byte, 4> data{};
    bytes::WriteU32Le(bytes::MutableByteView(data), 0, 0x12345678);
    ASSERT_EQ(data, (std::array<bytes::Byte, 4>{0x78, 0x56, 0x34, 0x12}));
}

TEST(TestBytes, writeBe_outOfBoundsIsNoOp)
{
    std::array<bytes::Byte, 1> data{0xAA};
    bytes::WriteU16Be(bytes::MutableByteView(data), 0, 0x1234);
    ASSERT_EQ(data, (std::array<bytes::Byte, 1>{0xAA}));
}

TEST(TestBytes, qByteArrayAppendU32Be_matchesSpanVersion)
{
    QByteArray out;
    bytes::appendU32Be(out, 0x12345678);
    ASSERT_EQ(out, QByteArray::fromHex("12345678"));
}

TEST(TestBytes, qByteArrayAppendU32Le_matchesSpanVersion)
{
    QByteArray out;
    bytes::appendU32Le(out, 0x12345678);
    ASSERT_EQ(out, QByteArray::fromHex("78563412"));
}

TEST(TestBytes, qByteArrayWriteU16Be_writesAtOffset)
{
    QByteArray out = QByteArray::fromHex("aaaaaaaa");
    bytes::writeU16Be(out, 1, 0x1234);
    ASSERT_EQ(out, QByteArray::fromHex("aa1234aa"));
}

TEST(TestBytes, qByteArrayWriteU32Le_writesLittleEndianBytes)
{
    QByteArray out(4, '\0');
    bytes::writeU32Le(out, 0, 0x12345678);
    ASSERT_EQ(out, QByteArray::fromHex("78563412"));
}

TEST(TestBytes, qByteArrayToHex_preservesTrailingSpaceFormat)
{
    ASSERT_EQ(bytes::toHex(QByteArray::fromHex("800102ff")), QString("80 01 02 ff "));
    ASSERT_EQ(bytes::toHex(QByteArray()), QString());
}

TEST(TestBytes, emptyConversions)
{
    const QByteArray empty;
    EXPECT_TRUE(bytes::view(empty).empty());
    EXPECT_TRUE(bytes::fromQByteArray(empty).empty());
    EXPECT_TRUE(bytes::toQByteArray(bytes::ByteView{}).isEmpty());
}

TEST(TestBytes, ownedRoundTripPreservesBinaryBytes)
{
    const QByteArray original = QByteArray::fromHex("00ff7f");
    const bytes::Bytes owned = bytes::fromQByteArray(original);
    ASSERT_THAT(owned, ElementsAre(0x00, 0xFF, 0x7F));
    EXPECT_EQ(bytes::toQByteArray(owned), original);
}

TEST(TestBytes, mutableViewAliasesSource)
{
    QByteArray original = QByteArray::fromHex("001122");
    auto borrowed = bytes::mutableView(original);
    borrowed[1] = 0xFF;
    EXPECT_EQ(original, QByteArray::fromHex("00ff22"));
}

TEST(TestBytes, fromQByteArrayCopiesSource)
{
    QByteArray original = QByteArray::fromHex("00ff");
    const bytes::Bytes owned = bytes::fromQByteArray(original);
    original[0] = '\x7f';
    EXPECT_THAT(owned, ElementsAre(0x00, 0xFF));
}

TEST(TestBytes, shortQByteArrayWriteIsNoOp)
{
    QByteArray original = QByteArray::fromHex("aabb");
    bytes::writeU32Be(original, 0, 0x12345678);
    EXPECT_EQ(original, QByteArray::fromHex("aabb"));
}
