#include "src/backend/flash/ecu/cvt_b6_chunk_exchange.h"

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

class ChunkChannel final : public uds::IUdsChannel
{
  public:
    Status Send(bytes::ByteView pdu, const ICancellationToken& cancellation) override
    {
        ++sends;
        EXPECT_EQ(bytes::Bytes(pdu.begin(), pdu.end()), (bytes::Bytes{0xB6, 0x08, 0x00, 0x00, 0x11, 0x22}));
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "channel cancelled");
        }
        return send_result;
    }
    Result<std::optional<bytes::Bytes>> Receive(std::chrono::milliseconds timeout, const ICancellationToken&) override
    {
        ++receives;
        EXPECT_EQ(timeout, 2000ms);
        return receive_result;
    }
    int sends = 0;
    int receives = 0;
    Status send_result;
    Result<std::optional<bytes::Bytes>> receive_result{std::optional<bytes::Bytes>{}};
};

TEST(CvtB6ChunkExchangeTest, SendsLiteralFrameAndAcceptsSilentOrUninspectedReply)
{
    for (bool present : {false, true})
    {
        ChunkChannel channel;
        FakeCancellationToken cancellation;
        if (present)
        {
            channel.receive_result = bytes::Bytes{0x7F, 0xB6, 0x22};
        }
        EXPECT_THAT(ExchangeCvtB6Chunk(channel, cancellation, 0x080000, bytes::Bytes{0x11, 0x22}),
                    fastecu::testing::IsOk());
        EXPECT_EQ(channel.sends, 1);
        EXPECT_EQ(channel.receives, 1);
    }
}

TEST(CvtB6ChunkExchangeTest, PropagatesSendReceiveAndCancellationErrors)
{
    for (int scenario : {0, 1, 2})
    {
        ChunkChannel channel;
        FakeCancellationToken cancellation(scenario == 2);
        if (scenario == 0)
        {
            channel.send_result = Fail(ErrorKind::kDisconnected, "send failed");
        }
        if (scenario == 1)
        {
            channel.receive_result = Fail(ErrorKind::kDisconnected, "receive failed");
        }
        const auto result = ExchangeCvtB6Chunk(channel, cancellation, 0x080000, bytes::Bytes{0x11, 0x22});
        ASSERT_THAT(result, fastecu::testing::IsErr(scenario == 2 ? ErrorKind::kCancelled : ErrorKind::kDisconnected));
        EXPECT_EQ(result.error().kind, scenario == 2 ? ErrorKind::kCancelled : ErrorKind::kDisconnected);
        EXPECT_EQ(result.error().detail, scenario == 0   ? "send failed"
                                         : scenario == 1 ? "receive failed"
                                                         : "channel cancelled");
        EXPECT_EQ(channel.sends, 1);
        EXPECT_EQ(channel.receives, scenario == 1 ? 1 : 0);
        EXPECT_EQ(cancellation.CheckCount(), 1U);
    }
}
} // namespace
} // namespace fastecu::flash
