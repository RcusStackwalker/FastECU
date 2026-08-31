#include "src/backend/protocol/uds/uds_client.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using testing::ElementsAre;
using testing::HasSubstr;

struct Fixture
{
    uds::ScriptedUdsChannel channel;
    FakeClock clock;
    RecordingEventSink events;
    FakeCancellationToken cancellation;

    uds::UdsClient client()
    {
        return uds::UdsClient(channel, clock, events);
    }
};

constexpr uds::ExchangePolicy kPolicy{
    .pre_read_delay_ms = 50, .read_timeout_ms = 500, .pending_timeout_ms = 3000, .max_pending_repeats = 3};

TEST(UdsClientTest, ReturnsThePositiveResponsePdu)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_THAT(*received, ElementsAre(0x50, 0x03));
    EXPECT_TRUE(f.channel.scriptConsumed());
}

TEST(UdsClientTest, SleepsForThePreReadDelayBeforeTheFirstRead)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.client();
    (void)client.request(request, kPolicy, f.cancellation);

    // FakeClock::sleep advances now_ by the requested duration, so the total
    // is the only observable: one 50 ms pre-read delay and nothing else.
    EXPECT_EQ(f.clock.now_, 50U);
    EXPECT_THAT(f.channel.timeouts_, ElementsAre(500));
}

TEST(UdsClientTest, AbsorbsOneResponsePendingAndReadsAgain)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_THAT(*received, ElementsAre(0x71, 0xE0));
    // Absorbed by re-reading only: exactly one transmission.
    EXPECT_EQ(f.channel.sendsConsumed(), 1U);
    // The pending read uses the longer pending timeout.
    EXPECT_THAT(f.channel.timeouts_, ElementsAre(500, 3000));
}

// The next two tests are a pair, and only the pair pins the retry boundary.
// kPolicy allows 3 repeats: the first fixes the largest run of pendings that
// is still absorbed, the second the smallest run that is not. Either one on
// its own is satisfied by an off-by-one limit, and neither name says so.
// Change them together; deleting one silently stops testing the boundary.
TEST(UdsClientTest, AbsorbsRepeatedResponsePending)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(f.channel.sendsConsumed(), 1U);
}

TEST(UdsClientTest, GivesUpAfterMaxPendingRepeats)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    for (int i = 0; i < 4; ++i)
    {
        f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    }

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Timeout);
    EXPECT_THAT(received.error().detail, HasSubstr("responsePending"));
    EXPECT_EQ(f.channel.sendsConsumed(), 1U);
}

TEST(UdsClientTest, DoesNotRetryBusyRepeatRequest)
{
    // The safety property: honoring 0x21 would mean re-TRANSMITTING, which is
    // unsafe for RequestDownload/TransferData/erase. It must surface as an
    // ordinary negative response after exactly one send.
    Fixture f;
    const bytes::Bytes request{0x36, 0x01};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x36, 0x21});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(f.channel.sendsConsumed(), 1U);
    EXPECT_TRUE(f.channel.scriptConsumed());
}

TEST(UdsClientTest, ReportsANegativeResponseWithItsNrcDescription)
{
    Fixture f;
    const bytes::Bytes request{0x27, 0x06};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x27, 0x35});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(received.error().detail, uds::describe(bytes::Bytes{0x7F, 0x27, 0x35}));
}

TEST(UdsClientTest, RejectsAResponseToADifferentService)
{
    Fixture f;
    const bytes::Bytes request{0x27, 0x05};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(received.error().detail, HasSubstr("0x27"));
    EXPECT_THAT(received.error().detail, HasSubstr("0x10"));
}

TEST(UdsClientTest, RejectsAMalformedResponse)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x10});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(received.error().detail, HasSubstr("malformed"));
}

TEST(UdsClientTest, ReportsATimeoutWhenNothingArrives)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueNoFrame();

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Timeout);
}

TEST(UdsClientTest, PropagatesAChannelErrorVerbatim)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueError(ErrorKind::Disconnected, "adapter closed");

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(received.error().detail, "adapter closed");
}

TEST(UdsClientTest, RefusesToSendWhenAlreadyCancelled)
{
    Fixture f;
    f.cancellation.set_cancelled(true);

    uds::UdsClient client = f.client();
    const auto received = client.request(bytes::Bytes{0x10, 0x03}, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(f.channel.sendsConsumed(), 0U);
}

TEST(UdsClientTest, RejectsAnEmptyRequest)
{
    Fixture f;

    uds::UdsClient client = f.client();
    const auto received = client.request({}, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Internal);
    EXPECT_EQ(f.channel.sendsConsumed(), 0U);
}

TEST(UdsClientTest, LogsOnceForEachAbsorbedPending)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.client();
    (void)client.request(request, kPolicy, f.cancellation);

    int pending_lines = 0;
    for (const auto& entry : f.events.logs)
    {
        if (entry.second.find("responsePending") != std::string::npos)
        {
            ++pending_lines;
        }
    }
    EXPECT_EQ(pending_lines, 1);
}

} // namespace
