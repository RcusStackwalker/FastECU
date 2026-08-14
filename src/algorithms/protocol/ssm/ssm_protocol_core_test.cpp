#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::ElementsAre;

TEST(SsmProtocolCorePortable, AddHeaderPrefixesTesterAndTarget)
{
    const bytes::Bytes payload{0xA0};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    ASSERT_GE(framed.size(), payload.size());
    EXPECT_EQ(framed[0], static_cast<bytes::Byte>(0x80));
}

TEST(SsmProtocolCore, AddHeaderBuildsTheFramedRequest)
{
    const bytes::Bytes payload{0xEF, 0x52};
    EXPECT_THAT(SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10),
                ElementsAre(0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3));
}

TEST(SsmProtocolCore, HasValidFrameAcceptsWhatAddHeaderProduces)
{
    const bytes::Bytes payload{0xEF, 0x52};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    EXPECT_TRUE(SsmProtocol::hasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasValidFrameRejectsACorruptedChecksum)
{
    bytes::Bytes framed{0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3};
    framed.back() = 0xC4;
    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}
