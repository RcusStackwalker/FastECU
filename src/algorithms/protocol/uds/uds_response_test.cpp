#include "src/algorithms/protocol/uds/uds_response.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>

namespace
{

using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

TEST(UdsResponseTest, ClassifiesAPositiveResponseAndRecoversTheRequestSid)
{
    const bytes::Bytes pdu{0x67, 0x01, 0x12, 0x34};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Positive);
    EXPECT_EQ(parsed.service, 0x27);
    EXPECT_THAT(parsed.data, ElementsAre(0x01, 0x12, 0x34));
    EXPECT_TRUE(parsed.matches(0x27));
    EXPECT_FALSE(parsed.matches(0x10));
    EXPECT_FALSE(parsed.isPending());
}

TEST(UdsResponseTest, ClassifiesAServiceOnlyPositiveResponse)
{
    const bytes::Bytes pdu{0x74};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Positive);
    EXPECT_EQ(parsed.service, 0x34);
    EXPECT_THAT(parsed.data, IsEmpty());
}

TEST(UdsResponseTest, ClassifiesANegativeResponse)
{
    const bytes::Bytes pdu{0x7F, 0x27, 0x35};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Negative);
    EXPECT_EQ(parsed.service, 0x27);
    EXPECT_EQ(parsed.nrc, 0x35);
    EXPECT_FALSE(parsed.isPending());
    EXPECT_FALSE(parsed.matches(0x27));
}

TEST(UdsResponseTest, RecognizesResponsePending)
{
    const bytes::Bytes pdu{0x7F, 0x31, 0x78};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Negative);
    EXPECT_TRUE(parsed.isPending());
}

TEST(UdsResponseTest, BusyRepeatRequestIsAnOrdinaryNegativeResponseNotPending)
{
    const bytes::Bytes pdu{0x7F, 0x36, 0x21};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Negative);
    EXPECT_EQ(parsed.nrc, uds::kNrcBusyRepeatRequest);
    EXPECT_FALSE(parsed.isPending());
}

TEST(UdsResponseTest, EmptyPduIsMalformed)
{
    EXPECT_EQ(uds::parseResponse({}).kind, uds::ResponseKind::Malformed);
}

TEST(UdsResponseTest, TruncatedNegativeResponseIsMalformed)
{
    const bytes::Bytes bare{0x7F};
    const bytes::Bytes no_nrc{0x7F, 0x27};

    EXPECT_EQ(uds::parseResponse(bare).kind, uds::ResponseKind::Malformed);
    EXPECT_EQ(uds::parseResponse(no_nrc).kind, uds::ResponseKind::Malformed);
}

TEST(UdsResponseTest, AByteBelowTheServiceOffsetIsMalformed)
{
    // 0x10 is a request SID, not a response: no positive response can be
    // below 0x40, so an echoed request is a protocol error, not a reply.
    const bytes::Bytes pdu{0x10, 0x03};
    EXPECT_EQ(uds::parseResponse(pdu).kind, uds::ResponseKind::Malformed);
}

TEST(UdsResponseTest, PayloadSkipsTheServiceByte)
{
    const bytes::Bytes pdu{0x63, 0x27, 0x41, 0x12};
    EXPECT_THAT(uds::payload(pdu), ElementsAre(0x27, 0x41, 0x12));
    EXPECT_THAT(uds::payload({}), IsEmpty());
}

TEST(UdsResponseTest, SubfunctionIsTheSecondByteWhenPresent)
{
    const bytes::Bytes pdu{0x50, 0x03};
    const bytes::Bytes service_only{0x50};

    EXPECT_EQ(uds::subfunction(pdu), std::optional<bytes::Byte>{0x03});
    EXPECT_EQ(uds::subfunction(service_only), std::nullopt);
    EXPECT_EQ(uds::subfunction({}), std::nullopt);
}

TEST(UdsResponseTest, DescribeDelegatesToTheSharedNrcTable)
{
    const bytes::Bytes pdu{0x7F, 0x27, 0x35};
    EXPECT_THAT(uds::describe(pdu), HasSubstr("Invalid key"));
}

} // namespace
