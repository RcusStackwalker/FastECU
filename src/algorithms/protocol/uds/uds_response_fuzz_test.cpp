#include "src/algorithms/protocol/uds/uds_response.h"

#include <cstdint>
#include <functional>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace
{
// True when `inner` lies within `outer`, so a view cannot outlive or escape
// the buffer it was parsed from.
bool IsWithin(bytes::ByteView inner, bytes::ByteView outer)
{
    const std::less_equal<const bytes::Byte *> before;
    return inner.empty() ||
           (before(outer.data(), inner.data()) && before(inner.data() + inner.size(), outer.data() + outer.size()));
}

// Any PDU classifies by the header's three ordered rules, and every view it
// yields stays inside the input.
void ClassifiesAnyPdu(const std::vector<std::uint8_t>& pdu)
{
    const bytes::ByteView view(pdu);
    const uds::Response response = uds::ParseResponse(view);
    EXPECT_TRUE(IsWithin(response.data, view));

    if (!pdu.empty() && pdu[0] == 0x7F)
    {
        EXPECT_EQ(response.kind, pdu.size() >= 3 ? uds::ResponseKind::kNegative : uds::ResponseKind::kMalformed);
    }
    else if (!pdu.empty() && pdu[0] >= 0x40)
    {
        EXPECT_EQ(response.kind, uds::ResponseKind::kPositive);
        EXPECT_EQ(response.service, pdu[0] - 0x40);
    }
    else
    {
        EXPECT_EQ(response.kind, uds::ResponseKind::kMalformed);
    }
    if (response.kind == uds::ResponseKind::kMalformed)
    {
        EXPECT_TRUE(response.data.empty());
    }

    const bytes::ByteView payload = uds::Payload(view);
    EXPECT_TRUE(IsWithin(payload, view));
    EXPECT_EQ(payload.size(), pdu.empty() ? 0 : pdu.size() - 1);
    EXPECT_EQ(uds::Subfunction(view).has_value(), pdu.size() >= 2);
    static_cast<void>(uds::Describe(view));
}
FUZZ_TEST(UdsResponseFuzz, ClassifiesAnyPdu);

// A positive response to a request matches that request's service and carries
// its data as the payload.
void MatchesThePositiveResponseToARequest(std::uint8_t sid, const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> pdu{static_cast<std::uint8_t>(sid + 0x40)};
    pdu.insert(pdu.end(), data.begin(), data.end());
    const uds::Response response = uds::ParseResponse(bytes::ByteView(pdu));
    EXPECT_TRUE(response.Matches(sid));
    EXPECT_FALSE(response.IsPending());
    EXPECT_EQ(bytes::Bytes(response.data.begin(), response.data.end()), data);
}
FUZZ_TEST(UdsResponseFuzz, MatchesThePositiveResponseToARequest)
    .WithDomains(fuzztest::InRange<std::uint8_t>(0x00, 0x3E), fuzztest::Arbitrary<std::vector<std::uint8_t>>());
} // namespace
