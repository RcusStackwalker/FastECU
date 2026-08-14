#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using testing::ElementsAre;

TEST(ScriptedUdsChannelTest, AcceptsAnExpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    const bytes::Bytes pdu{0x10, 0x03};
    channel.expectSend(pdu);

    EXPECT_TRUE(channel.send(pdu, cancellation).has_value());
    EXPECT_EQ(channel.sendsConsumed(), 1u);
}

TEST(ScriptedUdsChannelTest, RejectsAnUnexpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.expectSend(bytes::Bytes{0x10, 0x03});

    const fastecu::Status sent = channel.send(bytes::Bytes{0x10, 0x85}, cancellation);

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().kind, ErrorKind::Internal);
}

TEST(ScriptedUdsChannelTest, RejectsASendWithNoRemainingExpectation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;

    const fastecu::Status sent = channel.send(bytes::Bytes{0x3E}, cancellation);

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().kind, ErrorKind::Internal);
}

TEST(ScriptedUdsChannelTest, ReplaysQueuedReceivesInOrder)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.queueReceive(bytes::Bytes{0x50, 0x03});
    channel.queueNoFrame();
    channel.queueError(ErrorKind::Disconnected, "gone");

    const auto first = channel.receive(100, cancellation);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    EXPECT_THAT(**first, ElementsAre(0x50, 0x03));

    const auto second = channel.receive(100, cancellation);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->has_value());

    const auto third = channel.receive(100, cancellation);
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().kind, ErrorKind::Disconnected);
}

TEST(ScriptedUdsChannelTest, RecordsEveryReceiveTimeout)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.queueReceive(bytes::Bytes{0x50});
    channel.queueReceive(bytes::Bytes{0x50});

    (void)channel.receive(500, cancellation);
    (void)channel.receive(3000, cancellation);

    EXPECT_THAT(channel.timeouts_, ElementsAre(500, 3000));
    EXPECT_EQ(channel.last_timeout_ms_, 3000);
}

TEST(ScriptedUdsChannelTest, HonorsCancellation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    cancellation.set_cancelled(true);
    channel.expectSend(bytes::Bytes{0x3E});

    const fastecu::Status sent = channel.send(bytes::Bytes{0x3E}, cancellation);
    const auto received = channel.receive(100, cancellation);

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().kind, ErrorKind::Cancelled);
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Cancelled);
}

TEST(ScriptedUdsChannelTest, ScriptConsumedReflectsRemainingWork)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.expectSend(bytes::Bytes{0x3E});
    channel.queueReceive(bytes::Bytes{0x7E});

    EXPECT_FALSE(channel.scriptConsumed());
    (void)channel.send(bytes::Bytes{0x3E}, cancellation);
    (void)channel.receive(100, cancellation);
    EXPECT_TRUE(channel.scriptConsumed());
}

} // namespace
