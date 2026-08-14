#include "src/backend/flash/can_flash_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{

using bytes::Bytes;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::CanFlashUdsChannel;
using fastecu::flash::ScriptedCanFlashTransport;
using testing::ElementsAre;
using testing::HasSubstr;

constexpr std::uint32_t kRequestId = 0x7e0;
constexpr std::uint32_t kResponseId = 0x7e8;

TEST(CanFlashUdsChannelTest, PrependsTheRequestIdOnSend)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.expectWrite(Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const fastecu::Status sent = channel.send(Bytes{0x10, 0x03}, cancellation);

    EXPECT_TRUE(sent.has_value());
    EXPECT_EQ(transport.writesConsumed(), 1u);
}

TEST(CanFlashUdsChannelTest, StripsTheReplyIdOnReceive)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(received->has_value());
    EXPECT_THAT(**received, ElementsAre(0x50, 0x03));
}

TEST(CanFlashUdsChannelTest, PassesATimeoutThroughAsAnEmptyOptional)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queue_no_frame();

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_FALSE(received->has_value());
}

TEST(CanFlashUdsChannelTest, RejectsAFrameShorterThanTheEnvelope)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
}

TEST(CanFlashUdsChannelTest, RejectsAFrameFromAnUnexpectedReplyId)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(received.error().detail, HasSubstr("7e9"));
}

TEST(CanFlashUdsChannelTest, AcceptsAnEnvelopeOnlyFrameAsAnEmptyPdu)
{
    // A well-addressed frame carrying no PDU is not an envelope failure; the
    // client above classifies it as Malformed.
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07, 0xE8});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(received->has_value());
    EXPECT_TRUE((*received)->empty());
}

TEST(CanFlashUdsChannelTest, PropagatesATransportError)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queue_error(ErrorKind::Disconnected, "adapter closed");

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(received.error().detail, "adapter closed");
}

} // namespace
