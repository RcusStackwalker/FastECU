#include "src/backend/logging/logging_event_sink.h"
#include "src/backend/logging/portable_logging_protocol.h"
#include "src/backend/logging/logging_use_case.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{

using namespace fastecu::logging;

class ScriptedProtocol final : public LoggingProtocol
{
  public:
    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        start_call_poll_numbers.push_back(polls_completed);
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

    fastecu::Status start_result{};
    fastecu::Status stop_result{};
    int starts = 0;
    int stops = 0;
    int polls_completed = 0;
    std::deque<fastecu::Result<PollData>> polls;
    std::deque<fastecu::Status> start_results;
    std::vector<int> poll_timeouts;
    std::vector<int> start_call_poll_numbers;
};

class CancelsAfterPoll final : public fastecu::ICancellationToken
{
  public:
    explicit CancelsAfterPoll(const ScriptedProtocol& protocol, int poll_count = 1)
        : protocol_(protocol), poll_count_(poll_count)
    {
    }

    bool cancelled() const override
    {
        return protocol_.polls_completed >= poll_count_;
    }

  private:
    const ScriptedProtocol& protocol_;
    int poll_count_;
};

class AlwaysCancelled final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return true;
    }
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

class RecordingEventSink final : public fastecu::IEventSink
{
  public:
    void log(fastecu::LogLevel level, std::string_view message) override
    {
        logs.emplace_back(level, std::string(message));
    }

    void progress(int, int) override
    {
    }

    void notice(std::string_view) override
    {
    }

    std::vector<std::pair<fastecu::LogLevel, std::string>> logs;
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
    auto session = make_logging_session(LoggingProtocolId::Ssm, {channel("rpm", std::move(expression))},
                                        policy);
    EXPECT_TRUE(session);
    return std::move(*session);
}

LoggingSession make_valid_session()
{
    return session_with_policy({
        .poll_timeout_ms = 100,
        .car_silence_miss_threshold = 3,
        .reconnect_attempt_threshold = 2,
        .reconnect_retry_period = 0,
    });
}

fastecu::Status run_until_cancelled(const LoggingSession& session, ScriptedProtocol& protocol,
                                    RecordingLoggingSink& sink, int poll_count)
{
    CancelsAfterPoll token(protocol, poll_count);
    RecordingEventSink diagnostics;
    return LoggingUseCase{}.run(session, protocol, token, sink, diagnostics);
}

} // namespace

TEST(LoggingUseCaseTest, ConvertsAndEmitsOrderedSamplesThenCancels)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"rpm", "4000"}}});
    CancelsAfterPoll token(protocol);
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;

    auto result = LoggingUseCase{}.run(make_valid_session(), protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    ASSERT_EQ(sink.sample_batches.size(), 1u);
    ASSERT_EQ(sink.sample_batches[0].size(), 1u);
    EXPECT_EQ(sink.sample_batches[0][0].channel_id, "rpm");
    EXPECT_DOUBLE_EQ(sink.sample_batches[0][0].numeric_value, 4000.0);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running}));
    EXPECT_EQ(protocol.poll_timeouts, (std::vector{100}));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, PreCancellationDoesNotStartProtocol)
{
    ScriptedProtocol protocol;
    AlwaysCancelled token;
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;

    auto result = LoggingUseCase{}.run(make_valid_session(), protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(protocol.starts, 0);
    EXPECT_EQ(protocol.stops, 0);
}

TEST(LoggingUseCaseTest, PreservesSilenceThresholdAndReconnectCadence)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 2,
        .reconnect_attempt_threshold = 3,
        .reconnect_retry_period = 2,
    });
    ScriptedProtocol protocol;
    protocol.polls = {
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = true, .samples = {{"rpm", "7"}}},
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(session, protocol, sink, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running,
                                        LoggingState::CarNotResponding,
                                        LoggingState::Running}));
    EXPECT_EQ(protocol.start_call_poll_numbers, (std::vector{0, 3}));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, RetriesBadResponse)
{
    ScriptedProtocol protocol;
    protocol.polls = {
        fastecu::fail(fastecu::ErrorKind::BadResponse, "bad frame"),
        PollData{.responded = true, .samples = {{"rpm", "8"}}},
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(make_valid_session(), protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    ASSERT_EQ(sink.sample_batches.size(), 1u);
    EXPECT_EQ(sink.sample_batches[0][0].raw_value, "8");
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, RetriesFailedReconnectAtConfiguredCadence)
{
    auto session = session_with_policy({
        .poll_timeout_ms = 10,
        .car_silence_miss_threshold = 2,
        .reconnect_attempt_threshold = 3,
        .reconnect_retry_period = 2,
    });
    ScriptedProtocol protocol;
    protocol.start_results = {
        fastecu::Status{},
        fastecu::fail(fastecu::ErrorKind::BadResponse, "retry later"),
        fastecu::Status{},
    };
    protocol.polls = {
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
    };
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(session, protocol, sink, 5);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    EXPECT_EQ(protocol.start_call_poll_numbers, (std::vector{0, 3, 5}));
    EXPECT_EQ(sink.states, (std::vector{LoggingState::Running,
                                        LoggingState::CarNotResponding,
                                        LoggingState::Running}));
}

class TerminalPollErrorTest : public ::testing::TestWithParam<fastecu::ErrorKind>
{
};

TEST_P(TerminalPollErrorTest, TerminatesAndCleansUpOnce)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(fastecu::fail(GetParam(), "terminal"));
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(make_valid_session(), protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, GetParam());
    EXPECT_EQ(protocol.stops, 1);
}

INSTANTIATE_TEST_SUITE_P(LoggingUseCase, TerminalPollErrorTest,
                         ::testing::Values(fastecu::ErrorKind::InvalidConfig,
                                           fastecu::ErrorKind::Unsupported,
                                           fastecu::ErrorKind::Internal,
                                           fastecu::ErrorKind::Disconnected,
                                           fastecu::ErrorKind::Cancelled,
                                           fastecu::ErrorKind::Timeout));

TEST(LoggingUseCaseTest, ConversionInvalidConfigTerminates)
{
    auto session = session_with_policy(make_valid_session().policy(), "x/(x-1)");
    ScriptedProtocol protocol;
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"rpm", "1"}}});
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(session, protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, UnknownProtocolChannelTerminatesAsInternal)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"unknown", "1"}}});
    RecordingLoggingSink sink;

    auto result = run_until_cancelled(make_valid_session(), protocol, sink, 2);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, PrimaryErrorWinsOverStopFailure)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(fastecu::fail(fastecu::ErrorKind::Disconnected, "lost"));
    protocol.stop_result = fastecu::fail(fastecu::ErrorKind::Internal, "cleanup");
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    CancelsAfterPoll token(protocol, 2);

    auto result = LoggingUseCase{}.run(make_valid_session(), protocol, token, sink, diagnostics);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(protocol.stops, 1);
    ASSERT_EQ(diagnostics.logs.size(), 1u);
    EXPECT_EQ(diagnostics.logs[0].first, fastecu::LogLevel::Error);
}
