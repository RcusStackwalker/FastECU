#include "src/backend/flash/ecu/cvt_b6_chunk_exchange.h"

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

TEST(CvtB6ChunkExchangeTest, SendsLiteralFrameAndAcceptsSilentOrUninspectedReply)
{
    for (bool present : {false, true})
    {
        uds::ScriptedUdsChannel channel;
        channel.ExpectSend(bytes::Bytes{0xB6, 0x08, 0x00, 0x00, 0x11, 0x22});
        FakeCancellationToken cancellation;
        if (present)
        {
            channel.QueueReceive(bytes::Bytes{0x7F, 0xB6, 0x22});
        }
        else
        {
            channel.QueueNoFrame();
        }
        EXPECT_THAT(ExchangeCvtB6Chunk(channel, cancellation, 0x080000, bytes::Bytes{0x11, 0x22}),
                    fastecu::testing::IsOk());
        EXPECT_EQ(channel.SendCalls(), 1U);
        EXPECT_EQ(channel.timeouts.size(), 1U);
        EXPECT_EQ(channel.last_timeout, 2000ms);
        EXPECT_TRUE(channel.ScriptConsumed());
    }
}

TEST(CvtB6ChunkExchangeTest, PropagatesSendReceiveAndCancellationErrors)
{
    for (int scenario : {0, 1, 2})
    {
        uds::ScriptedUdsChannel channel;
        FakeCancellationToken cancellation(scenario == 2);
        channel.ExpectSend(bytes::Bytes{0xB6, 0x08, 0x00, 0x00, 0x11, 0x22},
                           scenario == 0 ? Status(Fail(ErrorKind::kDisconnected, "send failed")) : Status{});
        if (scenario == 1)
        {
            channel.QueueError(ErrorKind::kDisconnected, "receive failed");
        }
        const auto result = ExchangeCvtB6Chunk(channel, cancellation, 0x080000, bytes::Bytes{0x11, 0x22});
        ASSERT_THAT(result, fastecu::testing::IsErr(scenario == 2 ? ErrorKind::kCancelled : ErrorKind::kDisconnected));
        EXPECT_EQ(result.error().kind, scenario == 2 ? ErrorKind::kCancelled : ErrorKind::kDisconnected);
        EXPECT_EQ(result.error().detail, scenario == 0   ? "send failed"
                                         : scenario == 1 ? "receive failed"
                                                         : "scripted UDS send cancelled");
        EXPECT_EQ(channel.SendCalls(), 1U);
        EXPECT_EQ(channel.timeouts.size(), scenario == 1 ? 1U : 0U);
        if (scenario == 1)
        {
            EXPECT_EQ(channel.last_timeout, 2000ms);
        }
        EXPECT_EQ(channel.SendsConsumed(), scenario == 2 ? 0U : 1U);
        EXPECT_EQ(cancellation.CheckCount(), scenario == 1 ? 2U : 1U);
    }
}
} // namespace
} // namespace fastecu::flash
