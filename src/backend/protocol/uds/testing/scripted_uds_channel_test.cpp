#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{
using namespace std::chrono_literals;

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using testing::ElementsAre;

TEST(ScriptedUdsChannelTest, AcceptsAnExpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    const bytes::Bytes pdu{0x10, 0x03};
    channel.expectSend(pdu);

    EXPECT_THAT(channel.send(pdu, cancellation), fastecu::testing::IsOk());
    EXPECT_EQ(channel.sendsConsumed(), 1U);
}

TEST(ScriptedUdsChannelTest, RejectsAnUnexpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.expectSend(bytes::Bytes{0x10, 0x03});

    const fastecu::Status sent = channel.send(bytes::Bytes{0x10, 0x85}, cancellation);

    ASSERT_THAT(sent, fastecu::testing::IsErr(ErrorKind::Internal));
}

TEST(ScriptedUdsChannelTest, RejectsASendWithNoRemainingExpectation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;

    const fastecu::Status sent = channel.send(bytes::Bytes{0x3E}, cancellation);

    ASSERT_THAT(sent, fastecu::testing::IsErr(ErrorKind::Internal));
}

TEST(ScriptedUdsChannelTest, ReplaysQueuedReceivesInOrder)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.queueReceive(bytes::Bytes{0x50, 0x03});
    channel.queueNoFrame();
    channel.queueError(ErrorKind::Disconnected, "gone");

    const auto first = channel.receive(100ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_TRUE(first->has_value());
    EXPECT_THAT(**first, ElementsAre(0x50, 0x03));

    const auto second = channel.receive(100ms, cancellation);
    ASSERT_THAT(second, fastecu::testing::IsOk());
    EXPECT_FALSE(second->has_value());

    const auto third = channel.receive(100ms, cancellation);
    ASSERT_THAT(third, fastecu::testing::IsErr(ErrorKind::Disconnected));
}

TEST(ScriptedUdsChannelTest, RecordsEveryReceiveTimeout)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.queueReceive(bytes::Bytes{0x50});
    channel.queueReceive(bytes::Bytes{0x50});

    (void)channel.receive(500ms, cancellation);
    (void)channel.receive(3000ms, cancellation);

    EXPECT_THAT(channel.timeouts_, ElementsAre(500ms, 3000ms));
    EXPECT_EQ(channel.last_timeout_, 3000ms);
}

TEST(ScriptedUdsChannelTest, HonorsCancellation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    cancellation.set_cancelled(true);
    channel.expectSend(bytes::Bytes{0x3E});

    const fastecu::Status sent = channel.send(bytes::Bytes{0x3E}, cancellation);
    const auto received = channel.receive(100ms, cancellation);

    ASSERT_THAT(sent, fastecu::testing::IsErr(ErrorKind::Cancelled));
    ASSERT_THAT(received, fastecu::testing::IsErr(ErrorKind::Cancelled));
}

TEST(ScriptedUdsChannelTest, ScriptConsumedReflectsRemainingWork)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.expectSend(bytes::Bytes{0x3E});
    channel.queueReceive(bytes::Bytes{0x7E});

    EXPECT_FALSE(channel.scriptConsumed());
    (void)channel.send(bytes::Bytes{0x3E}, cancellation);
    EXPECT_FALSE(channel.scriptConsumed());
    (void)channel.receive(100ms, cancellation);
    EXPECT_TRUE(channel.scriptConsumed());
}

} // namespace
