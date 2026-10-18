#include "src/backend/flash/ecu/denso_beef_can_common.h"

#include <chrono>
#include <utility>
#include <vector>

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

TEST(DensoBeefCanCommonTest, BeefRequestFramesOpcodeAndPayloadLength)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03};
    const bytes::Bytes framed = BeefRequest(0xB6, payload);

    ASSERT_EQ(framed.size(), 8U);
    EXPECT_EQ(framed[0], 0xBE);
    EXPECT_EQ(framed[1], 0xEF);
    EXPECT_EQ(framed[2], 0x00);
    EXPECT_EQ(framed[3], 0x04);
    EXPECT_EQ(framed[4], 0xB6);
    EXPECT_EQ(framed[5], 0x01);
    EXPECT_EQ(framed[6], 0x02);
    EXPECT_EQ(framed[7], 0x03);
}

TEST(DensoBeefCanCommonTest, BeefRequestWithNoPayloadStillCountsTheOpcode)
{
    const bytes::Bytes framed = BeefRequest(0xA0);

    ASSERT_EQ(framed.size(), 5U);
    EXPECT_EQ(framed[3], 0x01);
    EXPECT_EQ(framed[4], 0xA0);
}

TEST(DensoBeefCanCommonTest, ElapsedMillisecondsReportsAtLeastOne)
{
    const std::chrono::steady_clock::time_point start{};

    EXPECT_EQ(ElapsedMilliseconds(start, start), 1U);
    EXPECT_EQ(ElapsedMilliseconds(start, start + 250ms), 250U);
}

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

TEST(DensoBeefCanCommonTest, BeefKernelPayloadReplacesThePaddedLastWordWithTheChecksum)
{
    // Five bytes pad to one eight-byte block, whose last word (holding the
    // kernel's fifth byte) becomes 0x5AA5A55A minus the sum of the others.
    const auto payload = BeefKernelPayload(bytes::Bytes{0x00, 0x00, 0x00, 0x01, 0xAA}, 8);

    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(*payload, EncryptPayload(bytes::Bytes{0x00, 0x00, 0x00, 0x01, 0x5A, 0xA5, 0xA5, 0x59}));
    EXPECT_FALSE(BeefKernelPayload({}, 8).has_value());
}

TEST(DensoBeefCanCommonTest, UploadBeefKernelBlocksEndsWithAnAddressOnlyBlock)
{
    struct Ctx
    {
        const ICancellationToken& cancellation;
    };
    FakeCancellationToken token;
    const Ctx ctx{token};
    std::vector<bytes::Bytes> requests;

    const Status uploaded = UploadBeefKernelBlocks(ctx, bytes::Bytes{0x01, 0x02, 0x03, 0x04}, 0x1000, 2,
                                                   [&requests](bytes::ByteView request) -> Status
                                                   {
                                                       requests.emplace_back(request.begin(), request.end());
                                                       return {};
                                                   });

    EXPECT_THAT(uploaded, IsOk());
    EXPECT_EQ(requests, (std::vector<bytes::Bytes>{{0xB6, 0x00, 0x10, 0x00, 0x01, 0x02},
                                                   {0xB6, 0x00, 0x10, 0x02, 0x03, 0x04},
                                                   {0xB6, 0x00, 0x10, 0x04}}));
}

} // namespace
} // namespace fastecu::flash
