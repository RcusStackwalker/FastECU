#include "src/backend/flash/ecu/uds_client_exchange_common.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

namespace fastecu::flash
{
namespace
{

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Pair;

constexpr uds::ExchangePolicy kPolicy{.read_timeout_ms = 500};

struct Fixture
{
    uds::ScriptedUdsChannel channel;
    FakeClock clock;
    RecordingEventSink events;
    FakeCancellationToken cancellation;
    uds::UdsClient client{channel, clock, events};

    UdsExchangeContext ctx()
    {
        return UdsExchangeContext{client, kPolicy, cancellation, events};
    }
};

TEST(ReportExchangeFailureTest, LogsRejectionWithPrefixAndReturnsFailureUnchanged)
{
    RecordingEventSink events;
    const Error failure{ErrorKind::BadResponse, "NRC 0x31"};

    const Error returned =
        report_exchange_failure(events, failure, "Wrong response from ECU: ", "the seed request");

    EXPECT_EQ(returned, failure);
    ASSERT_THAT(events.logs, ElementsAre(Pair(LogLevel::Error, "Wrong response from ECU: NRC 0x31")));
}

TEST(ReportExchangeFailureTest, LogsCancellationAsAnOperatorLineNotARejection)
{
    RecordingEventSink events;
    const Error failure{ErrorKind::Cancelled, ""};

    const Error returned =
        report_exchange_failure(events, failure, "Wrong response from ECU: ", "the erase trigger");

    EXPECT_EQ(returned, failure);
    ASSERT_EQ(events.logs.size(), 1u);
    EXPECT_EQ(events.logs[0].first, LogLevel::Warning);
    EXPECT_THAT(events.logs[0].second, HasSubstr("Cancelled by operator during the erase trigger"));
}

TEST(FatalRequestTest, ReturnsThePositiveResponseOnSuccess)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x10, 0x03});
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    const Result<bytes::Bytes> reply =
        fatal_request(f.ctx(), bytes::Bytes{0x10, 0x03}, "Wrong response from ECU: ", "the session request");

    ASSERT_TRUE(reply.has_value());
    EXPECT_THAT(*reply, ElementsAre(0x50, 0x03));
    EXPECT_THAT(f.events.logs, IsEmpty());
}

TEST(FatalRequestTest, LogsAndReturnsTheErrorOnFailure)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x10, 0x03});
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x10, 0x31});

    const Result<bytes::Bytes> reply =
        fatal_request(f.ctx(), bytes::Bytes{0x10, 0x03}, "Wrong response from ECU: ", "the session request");

    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, ErrorKind::BadResponse);
    ASSERT_EQ(f.events.logs.size(), 1u);
    EXPECT_EQ(f.events.logs[0].first, LogLevel::Error);
    EXPECT_THAT(f.events.logs[0].second, HasSubstr("Wrong response from ECU: "));
}

TEST(NonFatalQueryTest, LogsTheDecodedPayloadOnAMatchingSubfunction)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x09, 0x04});
    f.channel.queueReceive(bytes::Bytes{0x49, 0x04, 0xAB, 0xCD});

    non_fatal_query(f.ctx(), bytes::Bytes{0x09, 0x04}, bytes::Byte(0x04),
                    "Wrong response from ECU: ", "CAL ID");

    ASSERT_THAT(f.events.logs, ElementsAre(Pair(LogLevel::Info, "CAL ID: 04 ab cd ")));
}

TEST(NonFatalQueryTest, LogsAndDoesNotThrowOnAnExchangeFailure)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0xAA});
    f.channel.queueNoFrame();

    non_fatal_query(f.ctx(), bytes::Bytes{0xAA}, std::nullopt, "Wrong response from ECU: ", "ECU ID");

    ASSERT_EQ(f.events.logs.size(), 1u);
    EXPECT_EQ(f.events.logs[0].first, LogLevel::Error);
    EXPECT_THAT(f.events.logs[0].second, HasSubstr("Wrong response from ECU: "));
}

TEST(NonFatalQueryTest, LogsUnexpectedSubfunctionWithThePrefixAndDoesNotThrow)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x09, 0x04});
    f.channel.queueReceive(bytes::Bytes{0x49, 0x02});

    non_fatal_query(f.ctx(), bytes::Bytes{0x09, 0x04}, bytes::Byte(0x04),
                    "Wrong response from ECU: ", "CAL ID");

    ASSERT_THAT(f.events.logs,
                ElementsAre(Pair(LogLevel::Error, "Wrong response from ECU: unexpected subfunction")));
}

TEST(FatalQueryTest, ReturnsTheReplyOnAMatchingSingleBytePrefix)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x10, 0x43});
    f.channel.queueReceive(bytes::Bytes{0x50, 0x43});

    const Result<bytes::Bytes> reply =
        fatal_query(f.ctx(), bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x43}, "Wrong response from ECU: ",
                    "the bench diagnostic session", "unexpected session id",
                    "bench diagnostic session rejected");

    ASSERT_TRUE(reply.has_value());
    EXPECT_THAT(*reply, ElementsAre(0x50, 0x43));
    EXPECT_THAT(f.events.logs, IsEmpty());
}

TEST(FatalQueryTest, ReturnsTheReplyOnAMatchingMultiBytePrefix)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x31, 0x02, 0x02, 0x01});
    f.channel.queueReceive(bytes::Bytes{0x71, 0x02, 0x02, 0x03});

    const Result<bytes::Bytes> reply =
        fatal_query(f.ctx(), bytes::Bytes{0x31, 0x02, 0x02, 0x01}, bytes::Bytes{0x02, 0x02, 0x03},
                    "Wrong response from TCU: ", "the kernel alive re-check",
                    "unexpected alive-check response", "kernel alive re-check failed");

    ASSERT_TRUE(reply.has_value());
    EXPECT_THAT(*reply, ElementsAre(0x71, 0x02, 0x02, 0x03));
    EXPECT_THAT(f.events.logs, IsEmpty());
}

TEST(FatalQueryTest, LogsAndReturnsTheSendErrorOnExchangeFailure)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x10, 0x43});
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x10, 0x31});

    const Result<bytes::Bytes> reply =
        fatal_query(f.ctx(), bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x43}, "Wrong response from ECU: ",
                    "the bench diagnostic session", "unexpected session id",
                    "bench diagnostic session rejected");

    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, ErrorKind::BadResponse);
    ASSERT_EQ(f.events.logs.size(), 1u);
    EXPECT_THAT(f.events.logs[0].second, HasSubstr("Wrong response from ECU: "));
}

TEST(FatalQueryTest, LogsMismatchSummaryAndReturnsMismatchDetailOnAWrongPrefix)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x10, 0x43});
    f.channel.queueReceive(bytes::Bytes{0x50, 0x42});

    const Result<bytes::Bytes> reply =
        fatal_query(f.ctx(), bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x43}, "Wrong response from ECU: ",
                    "the bench diagnostic session", "unexpected session id",
                    "bench diagnostic session rejected");

    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(reply.error().detail, "bench diagnostic session rejected");
    ASSERT_THAT(f.events.logs,
                ElementsAre(Pair(LogLevel::Error, "Wrong response from ECU: unexpected session id")));
}

TEST(FatalQueryTest, TreatsAPayloadShorterThanMinPayloadSizeAsAMismatchEvenWithAMatchingPrefix)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x27, 0x01});
    f.channel.queueReceive(bytes::Bytes{0x67, 0x05, 0xAB});

    const Result<bytes::Bytes> reply =
        fatal_query(f.ctx(), bytes::Bytes{0x27, 0x01}, bytes::Bytes{0x05}, "Wrong response from ECU: ",
                    "the security access seed request", "unexpected security access seed",
                    "security seed rejected", 5);

    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, ErrorKind::BadResponse);
    ASSERT_THAT(f.events.logs, ElementsAre(Pair(LogLevel::Error,
                                                "Wrong response from ECU: unexpected security access seed")));
}

TEST(FatalQueryTest, AcceptsAPayloadAtLeastMinPayloadSizeWithAMatchingPrefix)
{
    Fixture f;
    f.channel.expectSend(bytes::Bytes{0x27, 0x01});
    f.channel.queueReceive(bytes::Bytes{0x67, 0x05, 0xAB, 0xCD, 0xEF, 0x01});

    const Result<bytes::Bytes> reply =
        fatal_query(f.ctx(), bytes::Bytes{0x27, 0x01}, bytes::Bytes{0x05}, "Wrong response from ECU: ",
                    "the security access seed request", "unexpected security access seed",
                    "security seed rejected", 5);

    ASSERT_TRUE(reply.has_value());
    EXPECT_THAT(*reply, ElementsAre(0x67, 0x05, 0xAB, 0xCD, 0xEF, 0x01));
    EXPECT_THAT(f.events.logs, IsEmpty());
}

} // namespace
} // namespace fastecu::flash
