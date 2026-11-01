#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/uds_client.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <tuple>

#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

namespace
{

using namespace std::chrono_literals;
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

    uds::UdsClient Client()
    {
        return uds::UdsClient(channel, clock, events);
    }
};

constexpr uds::ExchangePolicy kPolicy{
    .pre_read_delay = 50ms, .read_timeout = 500ms, .pending_timeout = 3000ms, .max_pending_repeats = 3};

TEST(UdsClientTest, ReturnsThePositiveResponsePdu)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.Client();
    const auto received = client.Request(request, kPolicy, f.cancellation);

    ASSERT_THAT(received, fastecu::testing::IsOk());
    EXPECT_THAT(*received, ElementsAre(0x50, 0x03));
    EXPECT_TRUE(f.channel.ScriptConsumed());
}

TEST(UdsClientTest, SleepsForThePreReadDelayBeforeTheFirstRead)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.Client();
    std::ignore = client.Request(request, kPolicy, f.cancellation);

    // FakeClock::sleep advances elapsed() by the requested duration, so the
    // total is the only observable: one 50 ms pre-read delay and nothing else.
    EXPECT_EQ(f.clock.Elapsed(), 50ms);
    EXPECT_THAT(f.channel.timeouts, ElementsAre(500ms));
}

TEST(UdsClientTest, AbsorbsOneResponsePendingAndReadsAgain)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.QueueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.Client();
    const auto received = client.Request(request, kPolicy, f.cancellation);

    ASSERT_THAT(received, fastecu::testing::IsOk());
    EXPECT_THAT(*received, ElementsAre(0x71, 0xE0));
    // Absorbed by re-reading only: exactly one transmission.
    EXPECT_EQ(f.channel.SendsConsumed(), 1U);
    // The pending read uses the longer pending timeout.
    EXPECT_THAT(f.channel.timeouts, ElementsAre(500ms, 3000ms));
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
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.QueueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.Client();
    ASSERT_THAT(client.Request(request, kPolicy, f.cancellation), fastecu::testing::IsOk());
    EXPECT_EQ(f.channel.SendsConsumed(), 1U);
}

TEST(UdsClientTest, GivesUpAfterMaxPendingRepeats)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.ExpectSend(request);
    for (int i = 0; i < 4; ++i)
    {
        f.channel.QueueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    }

    uds::UdsClient client = f.Client();
    const auto received = client.Request(request, kPolicy, f.cancellation);

    ASSERT_THAT(received, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_THAT(received, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("responsePending")));
    EXPECT_EQ(f.channel.SendsConsumed(), 1U);
}

TEST(UdsClientTest, DoesNotRetryBusyRepeatRequest)
{
    // The safety property: honoring 0x21 would mean re-TRANSMITTING, which is
    // unsafe for RequestDownload/TransferData/erase. It must surface as an
    // ordinary negative response after exactly one send.
    Fixture f;
    const bytes::Bytes request{0x36, 0x01};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x36, 0x21});

    uds::UdsClient client = f.Client();
    ASSERT_THAT(client.Request(request, kPolicy, f.cancellation), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(f.channel.SendsConsumed(), 1U);
    EXPECT_TRUE(f.channel.ScriptConsumed());
}

TEST(UdsClientTest, ReportsANegativeResponseWithItsNrcDescription)
{
    Fixture f;
    const bytes::Bytes request{0x27, 0x06};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x27, 0x35});

    uds::UdsClient client = f.Client();
    ASSERT_THAT(client.Request(request, kPolicy, f.cancellation),
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, uds::Describe(bytes::Bytes{0x7F, 0x27, 0x35})));
}

TEST(UdsClientTest, RejectsAResponseToADifferentService)
{
    Fixture f;
    const bytes::Bytes request{0x27, 0x05};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.Client();
    const auto received = client.Request(request, kPolicy, f.cancellation);

    ASSERT_THAT(received, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(received, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("0x27")));
    EXPECT_THAT(received, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("0x10")));
}

TEST(UdsClientTest, RejectsAMalformedResponse)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x10});

    uds::UdsClient client = f.Client();
    const auto received = client.Request(request, kPolicy, f.cancellation);

    ASSERT_THAT(received, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(received, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("malformed")));
}

TEST(UdsClientTest, ReportsATimeoutWhenNothingArrives)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.ExpectSend(request);
    f.channel.QueueNoFrame();

    uds::UdsClient client = f.Client();
    ASSERT_THAT(client.Request(request, kPolicy, f.cancellation), fastecu::testing::IsErr(ErrorKind::kTimeout));
}

TEST(UdsClientTest, PropagatesAChannelErrorVerbatim)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.ExpectSend(request);
    f.channel.QueueError(ErrorKind::kDisconnected, "adapter closed");

    uds::UdsClient client = f.Client();
    ASSERT_THAT(client.Request(request, kPolicy, f.cancellation),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, "adapter closed"));
}

TEST(UdsClientTest, RefusesToSendWhenAlreadyCancelled)
{
    Fixture f;
    f.cancellation.SetCancelled(true);

    uds::UdsClient client = f.Client();
    const auto result = client.Request(bytes::Bytes{0x10, 0x03}, kPolicy, f.cancellation);
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(result.error().cancellation_io_exposure, fastecu::CancellationIoExposure::kRefusedBeforeDelegation);
    EXPECT_EQ(f.channel.SendsConsumed(), 0U);
}

TEST(UdsClientTest, RejectsAnEmptyRequest)
{
    Fixture f;

    uds::UdsClient client = f.Client();
    ASSERT_THAT(client.Request({}, kPolicy, f.cancellation), fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_EQ(f.channel.SendsConsumed(), 0U);
}

TEST(UdsClientTest, LogsOnceForEachAbsorbedPending)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.ExpectSend(request);
    f.channel.QueueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.QueueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.Client();
    std::ignore = client.Request(request, kPolicy, f.cancellation);

    const auto pending_lines = std::ranges::count_if(
        f.events.logs, [](const auto& entry) { return entry.second.find("responsePending") != std::string::npos; });
    EXPECT_EQ(pending_lines, 1);
}

TEST(UdsClientTest, PreservesSendRefusalButPromotesCancellationAfterSuccessfulSend)
{
    const fastecu::Error refused{ErrorKind::kCancelled, "refused",
                                 fastecu::CancellationIoExposure::kRefusedBeforeDelegation};
    for (const int stage : {0, 1, 2})
    {
        SCOPED_TRACE(stage);
        Fixture f;
        f.channel.ExpectSend(bytes::Bytes{0x10, 0x03},
                             stage == 0 ? fastecu::Status(std::unexpected(refused)) : fastecu::Status{});
        if (stage == 2)
        {
            f.channel.QueueReceive(bytes::Bytes{0x7F, 0x10, 0x78});
        }
        if (stage != 0)
        {
            f.channel.QueueOutcome(std::unexpected(refused));
        }
        auto client = f.Client();
        const auto result = client.Request(bytes::Bytes{0x10, 0x03}, kPolicy, f.cancellation);
        ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, "refused"));
        EXPECT_EQ(result.error().cancellation_io_exposure,
                  stage == 0 ? fastecu::CancellationIoExposure::kRefusedBeforeDelegation
                             : fastecu::CancellationIoExposure::kMayHaveStarted);
        EXPECT_TRUE(f.channel.ScriptConsumed());
    }
}

TEST(UdsClientTest, PromotesPreReadDelayCancellationAfterSuccessfulSend)
{
    class CancelledClock final : public FakeClock
    {
      public:
        fastecu::Status Sleep(std::chrono::milliseconds, const fastecu::ICancellationToken&) override
        {
            return std::unexpected(fastecu::Error{ErrorKind::kCancelled, "delay refused",
                                                  fastecu::CancellationIoExposure::kRefusedBeforeDelegation});
        }
    } clock;
    Fixture f;
    f.channel.ExpectSend(bytes::Bytes{0x10, 0x03});
    uds::UdsClient client(f.channel, clock, f.events);
    const auto result = client.Request(bytes::Bytes{0x10, 0x03}, kPolicy, f.cancellation);
    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, "delay refused"));
    EXPECT_EQ(result.error().cancellation_io_exposure, fastecu::CancellationIoExposure::kMayHaveStarted);
    EXPECT_TRUE(f.channel.ScriptConsumed());
    EXPECT_THAT(f.channel.timeouts, ::testing::IsEmpty());
}

} // namespace
