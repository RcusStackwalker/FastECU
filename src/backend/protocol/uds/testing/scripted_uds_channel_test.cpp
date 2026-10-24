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
using ::testing::_;
using testing::ElementsAre;
using ::testing::Eq;
using ::testing::Optional;

TEST(ScriptedUdsChannelTest, AcceptsAnExpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    const bytes::Bytes pdu{0x10, 0x03};
    channel.ExpectSend(pdu);

    EXPECT_THAT(channel.Send(pdu, cancellation), fastecu::testing::IsOk());
    EXPECT_EQ(channel.SendsConsumed(), 1U);
}

TEST(ScriptedUdsChannelTest, ReplaysSendErrorsAndCountsEveryAttempt)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    const bytes::Bytes pdu{0x3E};
    channel.ExpectSend(pdu, fastecu::Fail(ErrorKind::kDisconnected, "send failed"));
    channel.ExpectSend(pdu);

    const auto failed = channel.Send(pdu, cancellation);
    ASSERT_THAT(failed, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_THAT(failed, fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("send failed")));
    EXPECT_EQ(channel.SendsConsumed(), 1U);
    EXPECT_FALSE(channel.ScriptConsumed());
    EXPECT_THAT(channel.Send(pdu, cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(channel.ScriptConsumed());
    EXPECT_EQ(channel.SendCalls(), 2U);

    EXPECT_THAT(channel.Send(pdu, cancellation), fastecu::testing::IsErr(ErrorKind::kInternal));
    cancellation.SetCancelled(true);
    EXPECT_THAT(channel.Send(pdu, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(channel.SendCalls(), 4U);
    EXPECT_EQ(channel.SendsConsumed(), 2U);
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
    ASSERT_THAT(*first, Optional(_));
    EXPECT_THAT(**first, ElementsAre(0x50, 0x03));

    const auto second = channel.Receive(100ms, cancellation);
    ASSERT_THAT(second, fastecu::testing::IsOk());
    EXPECT_THAT(*second, Eq(std::nullopt));

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
