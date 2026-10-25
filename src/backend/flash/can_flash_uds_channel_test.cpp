#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/can_flash_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{
using namespace std::chrono_literals;

using bytes::Bytes;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::CanFlashUdsChannel;
using fastecu::flash::ScriptedCanFlashTransport;
using ::testing::_;
using testing::ElementsAre;
using ::testing::Eq;
using testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Optional;

constexpr std::uint32_t kRequestId = 0x7e0;
constexpr std::uint32_t kResponseId = 0x7e8;

TEST(CanFlashUdsChannelTest, PrependsTheRequestIdOnSend)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.ExpectWrite(Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    EXPECT_THAT(channel.Send(Bytes{0x10, 0x03}, cancellation), fastecu::testing::IsOk());
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(CanFlashUdsChannelTest, StripsTheReplyIdOnReceive)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.QueueRead(Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.Receive(500ms, cancellation);

    ASSERT_THAT(received, fastecu::testing::IsOk());
    ASSERT_THAT(*received, Optional(_));
    EXPECT_THAT(**received, ElementsAre(0x50, 0x03));
}

TEST(CanFlashUdsChannelTest, PassesATimeoutThroughAsAnEmptyOptional)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.QueueNoFrame();

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.Receive(500ms, cancellation);

    ASSERT_THAT(received, fastecu::testing::IsOk());
    EXPECT_THAT(*received, Eq(std::nullopt));
}

TEST(CanFlashUdsChannelTest, RejectsAFrameShorterThanTheEnvelope)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.QueueRead(Bytes{0x00, 0x00, 0x07});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    ASSERT_THAT(channel.Receive(500ms, cancellation), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(CanFlashUdsChannelTest, RejectsAFrameFromAnUnexpectedReplyId)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.QueueRead(Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.Receive(500ms, cancellation);

    ASSERT_THAT(received, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(received, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("7e9")));
}

TEST(CanFlashUdsChannelTest, AcceptsAnEnvelopeOnlyFrameAsAnEmptyPdu)
{
    // A well-addressed frame carrying no PDU is not an envelope failure; the
    // client above classifies it as Malformed.
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.QueueRead(Bytes{0x00, 0x00, 0x07, 0xE8});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.Receive(500ms, cancellation);

    ASSERT_THAT(received, fastecu::testing::IsOk());
    ASSERT_THAT(*received, Optional(_));
    EXPECT_THAT(*(*received), IsEmpty());
}

TEST(CanFlashUdsChannelTest, PropagatesATransportError)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.QueueError(ErrorKind::kDisconnected, "adapter closed");

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    ASSERT_THAT(channel.Receive(500ms, cancellation),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, "adapter closed"));
}

} // namespace
