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
    const uds::Response parsed = uds::ParseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::kPositive);
    EXPECT_EQ(parsed.service, 0x27);
    EXPECT_THAT(parsed.data, ElementsAre(0x01, 0x12, 0x34));
    EXPECT_TRUE(parsed.Matches(0x27));
    EXPECT_FALSE(parsed.Matches(0x10));
    EXPECT_FALSE(parsed.IsPending());
}

TEST(UdsResponseTest, ClassifiesAServiceOnlyPositiveResponse)
{
    const bytes::Bytes pdu{0x74};
    const uds::Response parsed = uds::ParseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::kPositive);
    EXPECT_EQ(parsed.service, 0x34);
    EXPECT_THAT(parsed.data, IsEmpty());
}

TEST(UdsResponseTest, ClassifiesANegativeResponse)
{
    const bytes::Bytes pdu{0x7F, 0x27, 0x35};
    const uds::Response parsed = uds::ParseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::kNegative);
    EXPECT_EQ(parsed.service, 0x27);
    EXPECT_EQ(parsed.nrc, 0x35);
    EXPECT_FALSE(parsed.IsPending());
    EXPECT_FALSE(parsed.Matches(0x27));
}

TEST(UdsResponseTest, RecognizesResponsePending)
{
    const bytes::Bytes pdu{0x7F, 0x31, 0x78};
    const uds::Response parsed = uds::ParseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::kNegative);
    EXPECT_TRUE(parsed.IsPending());
}

TEST(UdsResponseTest, BusyRepeatRequestIsAnOrdinaryNegativeResponseNotPending)
{
    const bytes::Bytes pdu{0x7F, 0x36, 0x21};
    const uds::Response parsed = uds::ParseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::kNegative);
    EXPECT_EQ(parsed.nrc, uds::kNrcBusyRepeatRequest);
    EXPECT_FALSE(parsed.IsPending());
}

TEST(UdsResponseTest, EmptyPduIsMalformed)
{
    EXPECT_EQ(uds::ParseResponse({}).kind, uds::ResponseKind::kMalformed);
}

TEST(UdsResponseTest, TruncatedNegativeResponseIsMalformed)
{
    const bytes::Bytes bare{0x7F};
    const bytes::Bytes no_nrc{0x7F, 0x27};

    EXPECT_EQ(uds::ParseResponse(bare).kind, uds::ResponseKind::kMalformed);
    EXPECT_EQ(uds::ParseResponse(no_nrc).kind, uds::ResponseKind::kMalformed);
}

TEST(UdsResponseTest, AByteBelowTheServiceOffsetIsMalformed)
{
    // 0x10 is a request SID, not a response: no positive response can be
    // below 0x40, so an echoed request is a protocol error, not a reply.
    const bytes::Bytes pdu{0x10, 0x03};
    EXPECT_EQ(uds::ParseResponse(pdu).kind, uds::ResponseKind::kMalformed);
}

TEST(UdsResponseTest, PayloadSkipsTheServiceByte)
{
    const bytes::Bytes pdu{0x63, 0x27, 0x41, 0x12};
    EXPECT_THAT(uds::Payload(pdu), ElementsAre(0x27, 0x41, 0x12));
    EXPECT_THAT(uds::Payload({}), IsEmpty());
}

TEST(UdsResponseTest, SubfunctionIsTheSecondByteWhenPresent)
{
    const bytes::Bytes pdu{0x50, 0x03};
    const bytes::Bytes service_only{0x50};

    EXPECT_EQ(uds::Subfunction(pdu), std::optional<bytes::Byte>{0x03});
    EXPECT_EQ(uds::Subfunction(service_only), std::nullopt);
    EXPECT_EQ(uds::Subfunction({}), std::nullopt);
}

TEST(UdsResponseTest, DescribeDelegatesToTheSharedNrcTable)
{
    const bytes::Bytes pdu{0x7F, 0x27, 0x35};
    EXPECT_THAT(uds::Describe(pdu), HasSubstr("Invalid key"));
}

} // namespace
