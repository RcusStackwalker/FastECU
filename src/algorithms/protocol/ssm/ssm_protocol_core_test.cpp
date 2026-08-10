#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <gtest/gtest.h>

TEST(SsmProtocolCorePortable, AddHeaderPrefixesTesterAndTarget)
{
    const bytes::Bytes payload{0xA0};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    ASSERT_GE(framed.size(), payload.size());
    EXPECT_EQ(framed[0], static_cast<bytes::Byte>(0x80));
}
