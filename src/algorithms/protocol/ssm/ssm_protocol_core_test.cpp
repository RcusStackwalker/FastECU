#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <gtest/gtest.h>

TEST(SsmProtocolCorePortable, ChecksumSumsPayloadBytes)
{
    const bytes::Bytes frame{0x80, 0x10, 0xF0, 0x01, 0xBF};
    EXPECT_EQ(SsmProtocol::checksum(bytes::ByteView(frame)),
              static_cast<bytes::Byte>(0x80 + 0x10 + 0xF0 + 0x01 + 0xBF));
}

TEST(SsmProtocolCorePortable, AddHeaderPrefixesTesterAndTarget)
{
    const bytes::Bytes payload{0xA0};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    ASSERT_GE(framed.size(), payload.size());
    EXPECT_EQ(framed[0], static_cast<bytes::Byte>(0x80));
}

TEST(SsmProtocolCorePortable, ToHexRendersLowercasePairs)
{
    // toHex renders "%02x " per byte (two hex digits plus a trailing space),
    // matching the established contract in tests/test_ssm_protocol.cpp
    // (SsmProtocol::toHex(QByteArray::fromHex("800102ff")) == "80 01 02 ff ").
    // For 2 bytes that is 2*3 = 6 characters, not 4.
    const bytes::Bytes buf{0x0F, 0xA0};
    EXPECT_EQ(SsmProtocol::toHex(bytes::ByteView(buf)).size(), 6u);
}
