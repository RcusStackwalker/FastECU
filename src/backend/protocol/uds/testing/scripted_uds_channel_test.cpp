#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <tuple>

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
    channel.ExpectSend(pdu);

    EXPECT_THAT(channel.Send(pdu, cancellation), fastecu::testing::IsOk());
    EXPECT_EQ(channel.SendsConsumed(), 1U);
}

TEST(ScriptedUdsChannelTest, RejectsAnUnexpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.ExpectSend(bytes::Bytes{0x10, 0x03});

    ASSERT_THAT(channel.Send(bytes::Bytes{0x10, 0x85}, cancellation), fastecu::testing::IsErr(ErrorKind::kInternal));
}

TEST(ScriptedUdsChannelTest, RejectsASendWithNoRemainingExpectation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;

    ASSERT_THAT(channel.Send(bytes::Bytes{0x3E}, cancellation), fastecu::testing::IsErr(ErrorKind::kInternal));
}

TEST(ScriptedUdsChannelTest, ReplaysQueuedReceivesInOrder)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.QueueReceive(bytes::Bytes{0x50, 0x03});
    channel.QueueNoFrame();
    channel.QueueError(ErrorKind::kDisconnected, "gone");

    const auto first = channel.Receive(100ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_TRUE(first->has_value());
    EXPECT_THAT(**first, ElementsAre(0x50, 0x03));

    const auto second = channel.Receive(100ms, cancellation);
    ASSERT_THAT(second, fastecu::testing::IsOk());
    EXPECT_FALSE(second->has_value());

    ASSERT_THAT(channel.Receive(100ms, cancellation), fastecu::testing::IsErr(ErrorKind::kDisconnected));
}

TEST(ScriptedUdsChannelTest, RecordsEveryReceiveTimeout)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.QueueReceive(bytes::Bytes{0x50});
    channel.QueueReceive(bytes::Bytes{0x50});

    std::ignore = channel.Receive(500ms, cancellation);
    std::ignore = channel.Receive(3000ms, cancellation);

    EXPECT_THAT(channel.timeouts, ElementsAre(500ms, 3000ms));
    EXPECT_EQ(channel.last_timeout, 3000ms);
}

TEST(ScriptedUdsChannelTest, HonorsCancellation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    cancellation.SetCancelled(true);
    channel.ExpectSend(bytes::Bytes{0x3E});

    ASSERT_THAT(channel.Send(bytes::Bytes{0x3E}, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
    ASSERT_THAT(channel.Receive(100ms, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
}

TEST(ScriptedUdsChannelTest, ScriptConsumedReflectsRemainingWork)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.ExpectSend(bytes::Bytes{0x3E});
    channel.QueueReceive(bytes::Bytes{0x7E});

    EXPECT_FALSE(channel.ScriptConsumed());
    std::ignore = channel.Send(bytes::Bytes{0x3E}, cancellation);
    EXPECT_FALSE(channel.ScriptConsumed());
    std::ignore = channel.Receive(100ms, cancellation);
    EXPECT_TRUE(channel.ScriptConsumed());
}

} // namespace
