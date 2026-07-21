#include "src/algorithms/protocol/bytes.h"

#include <gtest/gtest.h>

TEST(BytesPortable, ReadsBigEndianWidths)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::readU16Be(view), 0x1234u);
    EXPECT_EQ(bytes::readU24Be(view), 0x123456u);
    EXPECT_EQ(bytes::readU32Be(view), 0x12345678u);
}

TEST(BytesPortable, ReadsLittleEndianWidths)
{
    const bytes::Bytes buf{0x12, 0x34, 0x56, 0x78};
    const bytes::ByteView view(buf);
    EXPECT_EQ(bytes::readU16Le(view), 0x3412u);
    EXPECT_EQ(bytes::readU24Le(view), 0x563412u);
    EXPECT_EQ(bytes::readU32Le(view), 0x78563412u);
}

TEST(BytesPortable, AppendsBigEndianWidths)
{
    bytes::Bytes out;
    bytes::appendU16Be(out, 0x1234);
    bytes::appendU24Be(out, 0x563412);
    EXPECT_EQ(out, (bytes::Bytes{0x12, 0x34, 0x56, 0x34, 0x12}));
}

TEST(BytesPortable, Sums8OverFullView)
{
    const bytes::Bytes buf{0x01, 0x02, 0x03, 0xFF};
    EXPECT_EQ(bytes::sum8(bytes::ByteView(buf)), static_cast<bytes::Byte>(0x05));
}
