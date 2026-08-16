#include "src/algorithms/protocol/uds/uds_pdu.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using testing::ElementsAre;
using testing::IsEmpty;

TEST(UdsPduTest, PositiveResponseAddsTheServiceOffset)
{
    EXPECT_EQ(uds::positiveResponse(0x10), 0x50);
    EXPECT_EQ(uds::positiveResponse(0x27), 0x67);
    EXPECT_EQ(uds::positiveResponse(0x3B), 0x7B);
}

TEST(UdsPduTest, RequestFromPositiveIsTheInverse)
{
    EXPECT_EQ(uds::requestFromPositive(0x50), 0x10);
    EXPECT_EQ(uds::requestFromPositive(0x67), 0x27);
}

TEST(UdsPduTest, StructuralNrcConstantsMatchTheStandard)
{
    EXPECT_EQ(uds::kNegativeResponse, 0x7F);
    EXPECT_EQ(uds::kPositiveResponseOffset, 0x40);
    EXPECT_EQ(uds::kNrcResponsePending, 0x78);
    EXPECT_EQ(uds::kNrcBusyRepeatRequest, 0x21);
}

TEST(UdsPduTest, BuildsAServiceOnlyRequest)
{
    EXPECT_THAT(uds::buildRequest(0x3E), ElementsAre(0x3E));
}

TEST(UdsPduTest, BuildsASubfunctionRequest)
{
    EXPECT_THAT(uds::buildRequest(0x10, bytes::Byte{0x03}), ElementsAre(0x10, 0x03));
}

TEST(UdsPduTest, BuildsADataCarryingRequest)
{
    const bytes::Bytes data{0x12, 0x34, 0x56};
    EXPECT_THAT(uds::buildRequest(0x23, bytes::ByteView(data)), ElementsAre(0x23, 0x12, 0x34, 0x56));
}

TEST(UdsPduTest, BuildsASubfunctionAndDataRequest)
{
    const bytes::Bytes key{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_THAT(uds::buildRequest(0x27, bytes::Byte{0x06}, bytes::ByteView(key)),
                ElementsAre(0x27, 0x06, 0xDE, 0xAD, 0xBE, 0xEF));
}

TEST(UdsPduTest, EmptyDataYieldsAServiceOnlyFrame)
{
    EXPECT_THAT(uds::buildRequest(0x23, bytes::ByteView{}), ElementsAre(0x23));
}

TEST(UdsPduTest, BuildersNeverReturnAnEmptyFrame)
{
    EXPECT_THAT(uds::buildRequest(0x00), testing::Not(IsEmpty()));
}

} // namespace
