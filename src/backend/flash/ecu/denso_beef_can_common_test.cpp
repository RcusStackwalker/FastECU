#include "src/backend/flash/ecu/denso_beef_can_common.h"

#include <chrono>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{

using namespace std::chrono_literals;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::StrEq;

TEST(DensoBeefCanCommonTest, CancelledIfRequestedReportsTheCallerDetail)
{
    struct Ctx
    {
        const ICancellationToken& cancellation;
    };
    FakeCancellationToken token;
    const Ctx ctx{token};

    EXPECT_THAT(CancelledIfRequested(ctx, "cancelled before CAN request"), IsOk());

    token.SetCancelled(true);
    const Status cancelled = CancelledIfRequested(ctx, "cancelled before CAN request");
    EXPECT_THAT(cancelled, IsErrWith(ErrorKind::kCancelled, StrEq("cancelled before CAN request")));
}

TEST(DensoBeefCanCommonTest, ParseBeefBoundsEveryDeclaredField)
{
    const auto parse = [](bytes::Bytes pdu) { return ParseBeef(pdu); };
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00}), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response envelope")));
    EXPECT_THAT(parse({0x12, 0x34, 0x00, 0x01, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("wrong BEEF response marker")));
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00, 0x09, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("invalid BEEF response length")));
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00, 0x00, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("invalid BEEF response length")));
    for (const auto& [pdu, payload_size] : {std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0xB6}, 0U},
                                            std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0xB6, 0xAA, 0xFF}, 1U}})
    {
        SCOPED_TRACE(::testing::PrintToString(pdu));
        const auto parsed = ParseBeef(pdu);
        ASSERT_THAT(parsed, IsOk());
        EXPECT_EQ(parsed->opcode, 0xB6);
        EXPECT_EQ(parsed->payload.size(), payload_size);
    }
}

TEST(DensoBeefCanCommonTest, SendChannelRequestSleepsOnlyForAPositiveDelay)
{
    struct Channel
    {
        Status Send(bytes::ByteView, const ICancellationToken&)
        {
            return {};
        }
    };
    struct Ctx
    {
        Channel channel;
        FakeClock clock;
        FakeCancellationToken cancellation;
    } ctx;
    ctx.clock.SetSleepAdvance(5ms);

    EXPECT_THAT(SendChannelRequest(ctx, bytes::Bytes{0x01}, 0ms), IsOk());
    EXPECT_EQ(ctx.clock.Elapsed(), 0ms);
    EXPECT_THAT(SendChannelRequest(ctx, bytes::Bytes{0x01}, 10ms), IsOk());
    EXPECT_EQ(ctx.clock.Elapsed(), 5ms);
}

TEST(DensoBeefCanCommonTest, UppercaseHexCompactHasNoSeparators)
{
    EXPECT_EQ(UppercaseHexCompact(bytes::Bytes{0x0A, 0xFF, 0x00}), "0AFF00");
}

} // namespace
} // namespace fastecu::flash
