#include "src/backend/logging/logging_event_sink.h"
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/logging_use_case.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{

using namespace fastecu::logging;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;

class ScriptedProtocol final : public LoggingProtocol
{
  public:
    explicit ScriptedProtocol(FakeClock& clock) : clock_(clock)
    {
    }

    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        start_call_poll_numbers.push_back(polls_completed);
        start_call_times_ms.push_back(clock_.now_ms());
        ++starts;
        if (!start_results.empty())
        {
            auto result = std::move(start_results.front());
            start_results.pop_front();
            return result;
        }
        return start_result;
    }

    fastecu::Result<PollData> poll(int timeout_ms, const fastecu::ICancellationToken&) override
    {
        poll_timeouts.push_back(timeout_ms);
        ++polls_completed;
        const auto delta = static_cast<std::uint64_t>(std::max(timeout_ms, 0));
        const auto max = std::numeric_limits<std::uint64_t>::max();
        clock_.now_ = delta > max - clock_.now_ ? max : clock_.now_ + delta;
        if (polls.empty())
        {
            return PollData{.responded = false};
        }
        auto result = std::move(polls.front());
        polls.pop_front();
        return result;
    }

    fastecu::Status stop() override
    {
        ++stops;
        return stop_result;
    }

    fastecu::Status start_result;
    fastecu::Status stop_result;
    int starts = 0;
    int stops = 0;
    int polls_completed = 0;
    std::deque<fastecu::Result<PollData>> polls;
    std::deque<fastecu::Status> start_results;
    std::vector<int> poll_timeouts;
    std::vector<int> start_call_poll_numbers;
    std::vector<std::uint64_t> start_call_times_ms;

  private:
    FakeClock& clock_;
};

class RecordingLoggingSink final : public ILoggingEventSink
{
  public:
    void state_changed(LoggingState state) override
    {
        states.push_back(state);
    }

    void samples(std::span<const LogSample> values) override
    {
        sample_batches.emplace_back(values.begin(), values.end());
    }

    std::vector<LoggingState> states;
    std::vector<std::vector<LogSample>> sample_batches;
};

LoggingChannel channel(std::string id, std::string expression = "x")
{
    return LoggingChannel{
        .id = std::move(id),
        .address = 0x10,
        .length = 2,
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = std::move(expression),
        .unit = "rpm",
        .decimal_precision = 2,
    };
}

LoggingSession session_with_policy(LoggingPolicy policy, std::string expression = "x")
{
    auto session = make_logging_session(LoggingProtocolId::Ssm, {channel("rpm", std::move(expression))}, policy);
    EXPECT_TRUE(session);
    return std::move(*session);
}

LoggingSession make_valid_session()
{
    return session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 2,
        .reconnect_initial_delay_ms = 20,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = 3,
    });
}

fastecu::Status run_until_cancelled(FakeClock& clock, const LoggingSession& session, ScriptedProtocol& protocol,
                                    RecordingLoggingSink& sink, int poll_count)
{
    FakeCancellationToken token;
    token.set_predicate([&protocol, poll_count] { return protocol.polls_completed >= poll_count; });
    RecordingEventSink diagnostics;
    return LoggingUseCase(clock).run(session, protocol, token, sink, diagnostics);
}

} // namespace

TEST(LoggingUseCaseTest, ConvertsAndEmitsOrderedSamplesThenCancels)
{
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"rpm", "4000"}}});
    FakeCancellationToken token;
    token.set_predicate([&protocol] { return protocol.polls_completed >= 1; });
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;

    auto result = LoggingUseCase(clock).run(make_valid_session(), protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    ASSERT_EQ(sink.sample_batches.size(), 1U);
    ASSERT_EQ(sink.sample_batches[0].size(), 1U);
    EXPECT_EQ(sink.sample_batches[0][0].channel_id, "rpm");
    EXPECT_DOUBLE_EQ(sink.sample_batches[0][0].numeric_value, 4000.0);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running}));
    EXPECT_EQ(protocol.poll_timeouts, (std::vector{10}));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, PreCancellationDoesNotStartProtocol)
{
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    FakeCancellationToken token(true);
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;

    auto result = LoggingUseCase(clock).run(make_valid_session(), protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(protocol.starts, 0);
    EXPECT_EQ(protocol.stops, 0);
}

TEST(LoggingUseCaseTest, EmitsSilenceAtMissThresholdAndWaitsForInitialDeadline)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 2,
        .reconnect_initial_delay_ms = 20,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = std::nullopt,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls = {
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, session, protocol, sink, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running, LoggingState::CarNotResponding}));
    EXPECT_EQ(protocol.start_call_times_ms, (std::vector<std::uint64_t>{0, 40}));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, RetriesBadResponse)
{
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls = {
        fastecu::fail(fastecu::ErrorKind::BadResponse, "bad frame"),
        PollData{.responded = true, .samples = {{"rpm", "8"}}},
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, make_valid_session(), protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    ASSERT_EQ(sink.sample_batches.size(), 1U);
    EXPECT_EQ(sink.sample_batches[0][0].raw_value, "8");
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, SpacesLaterRestartsByElapsedTime)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 3,
        .car_silence_miss_threshold = 2,
        .reconnect_initial_delay_ms = 20,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = std::nullopt,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, session, protocol, sink, 13);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    ASSERT_EQ(protocol.start_call_times_ms, (std::vector<std::uint64_t>{0, 27, 39}));
    EXPECT_GE(protocol.start_call_times_ms[2] - protocol.start_call_times_ms[1], 10U);
}

TEST(LoggingUseCaseTest, CountsSuccessfulAndBadResponseRestartsTowardLimit)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 1,
        .reconnect_initial_delay_ms = 0,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = 2,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.start_results = {
        fastecu::Status{},
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::BadResponse, "second attempt failed"),
    };
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    FakeCancellationToken token;

    auto result = LoggingUseCase(clock).run(session, protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
    EXPECT_EQ(result.error().detail, "logging reconnect attempts exhausted");
    EXPECT_EQ(protocol.starts, 3);
}

TEST(LoggingUseCaseTest, ValidSampleResetsAttemptsAndReturnsToRunning)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 2,
        .reconnect_initial_delay_ms = 0,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = 1,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls = {
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = true, .samples = {{"rpm", "7"}}},
        PollData{.responded = false},
        PollData{.responded = false},
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, session, protocol, sink, 5);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(protocol.starts, 3);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running, LoggingState::CarNotResponding, LoggingState::Running,
                                        LoggingState::CarNotResponding}));
}

TEST(LoggingUseCaseTest, EmptyRespondedPollDoesNotResetReconnectState)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 1,
        .reconnect_initial_delay_ms = 0,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = 1,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.start_results = {
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::BadResponse, "retained retry failure"),
    };
    protocol.polls = {
        PollData{.responded = false},
        PollData{.responded = true, .samples = {}},
        PollData{.responded = false},
    };
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    FakeCancellationToken token;
    token.set_predicate([&protocol] { return protocol.polls_completed >= 3; });

    auto result = LoggingUseCase(clock).run(session, protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
    EXPECT_EQ(result.error().detail, "logging reconnect attempts exhausted");
    EXPECT_EQ(protocol.starts, 2);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running, LoggingState::CarNotResponding}));
    EXPECT_TRUE(sink.sample_batches.empty());
    ASSERT_EQ(diagnostics.logs.size(), 1U);
    EXPECT_EQ(diagnostics.logs.back().second, "retained retry failure");
}

TEST(LoggingUseCaseTest, ExhaustsThreeAttemptsAndLogsMostRecentRetryFailure)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 1,
        .reconnect_initial_delay_ms = 0,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = 3,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.start_results = {
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::BadResponse, "first retry failure"),
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::BadResponse, "most recent retry failure"),
    };
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    FakeCancellationToken token;

    auto result = LoggingUseCase(clock).run(session, protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
    EXPECT_EQ(result.error().detail, "logging reconnect attempts exhausted");
    EXPECT_EQ(protocol.starts, 4);
    ASSERT_EQ(diagnostics.logs.size(), 1U);
    EXPECT_EQ(diagnostics.logs.back().first, fastecu::LogLevel::Error);
    EXPECT_EQ(diagnostics.logs.back().second, "most recent retry failure");
}

TEST(LoggingUseCaseTest, UnlimitedAttemptsContinuePastThreeUntilCancellation)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 1,
        .reconnect_initial_delay_ms = 0,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = std::nullopt,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.start_results = {
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::BadResponse, "one"),
        fastecu::fail(fastecu::ErrorKind::BadResponse, "two"),
        fastecu::fail(fastecu::ErrorKind::BadResponse, "three"),
        fastecu::fail(fastecu::ErrorKind::BadResponse, "four"),
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, session, protocol, sink, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(protocol.starts, 5);
}

TEST(LoggingUseCaseTest, TerminalRestartErrorReturnsImmediately)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 1,
        .reconnect_initial_delay_ms = 0,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = 3,
    });
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.start_results = {
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::Disconnected, "transport lost"),
    };
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    FakeCancellationToken token;

    auto result = LoggingUseCase(clock).run(session, protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "transport lost");
    EXPECT_EQ(protocol.starts, 2);
    EXPECT_EQ(protocol.polls_completed, 1);
}

TEST(LoggingUseCaseTest, OverflowedPositiveReconnectDeadlineNeverBecomesDueAtMaximumClockValue)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 1,
        .car_silence_miss_threshold = 1,
        .reconnect_initial_delay_ms = 20,
        .reconnect_period_ms = 10,
        .max_reconnect_attempts = std::nullopt,
    });
    FakeClock clock;
    clock.now_ = std::numeric_limits<std::uint64_t>::max() - 5;
    ScriptedProtocol protocol(clock);
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, session, protocol, sink, 7);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(clock.now_, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(protocol.polls_completed, 7);
    EXPECT_EQ(protocol.start_call_times_ms,
              (std::vector<std::uint64_t>{std::numeric_limits<std::uint64_t>::max() - 5}));
}

class TerminalPollErrorTest : public ::testing::TestWithParam<fastecu::ErrorKind>
{
};

TEST_P(TerminalPollErrorTest, TerminatesAndCleansUpOnce)
{
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls.push_back(fastecu::fail(GetParam(), "terminal"));
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, make_valid_session(), protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, GetParam());
    EXPECT_EQ(protocol.stops, 1);
}

INSTANTIATE_TEST_SUITE_P(LoggingUseCase, TerminalPollErrorTest,
                         ::testing::Values(fastecu::ErrorKind::InvalidConfig, fastecu::ErrorKind::Unsupported,
                                           fastecu::ErrorKind::Internal, fastecu::ErrorKind::Disconnected,
                                           fastecu::ErrorKind::Cancelled, fastecu::ErrorKind::Timeout));

TEST(LoggingUseCaseTest, ConversionInvalidConfigTerminates)
{
    auto session = session_with_policy(make_valid_session().policy(), "x/(x-1)");
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"rpm", "1"}}});
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, session, protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, UnknownProtocolChannelTerminatesAsInternal)
{
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"unknown", "1"}}});
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(clock, make_valid_session(), protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, PrimaryErrorWinsOverStopFailure)
{
    FakeClock clock;
    ScriptedProtocol protocol(clock);
    protocol.polls.push_back(fastecu::fail(fastecu::ErrorKind::Disconnected, "lost"));
    protocol.stop_result = fastecu::fail(fastecu::ErrorKind::Internal, "cleanup");
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    FakeCancellationToken token;
    token.set_predicate([&protocol] { return protocol.polls_completed >= 2; });

    auto result = LoggingUseCase(clock).run(make_valid_session(), protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(protocol.stops, 1);
    ASSERT_EQ(diagnostics.logs.size(), 1U);
    EXPECT_EQ(diagnostics.logs[0].first, fastecu::LogLevel::Error);
}
