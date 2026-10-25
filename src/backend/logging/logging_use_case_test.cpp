#include <array>
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logging_event_sink.h"
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/logging_use_case.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"

#include <chrono>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::IsEmpty;

namespace
{

using namespace fastecu::logging;
using namespace std::chrono_literals;
using fastecu::FakeCancellationToken;
using fastecu::RecordingEventSink;

class ScriptedProtocol final : public LoggingProtocol
{
  public:
    fastecu::Status Start(const fastecu::ICancellationToken&) override
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

    fastecu::Result<PollData> Poll(std::chrono::milliseconds timeout, const fastecu::ICancellationToken&) override
    {
        poll_timeouts.push_back(timeout);
        ++polls_completed;
        if (polls.empty())
        {
            return PollData{.responded = false};
        }
        auto result = std::move(polls.front());
        polls.pop_front();
        return result;
    }

    fastecu::Status Stop() override
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
    std::vector<std::chrono::milliseconds> poll_timeouts;
    std::vector<int> start_call_poll_numbers;
};

class RecordingLoggingSink final : public ILoggingEventSink
{
  public:
    void StateChanged(LoggingState state) override
    {
        states.push_back(state);
    }

    void Samples(std::span<const LogSample> values) override
    {
        sample_batches.emplace_back(values.begin(), values.end());
    }

    std::vector<LoggingState> states;
    std::vector<std::vector<LogSample>> sample_batches;
};

LoggingChannel Channel(std::string id, std::string expression = "x")
{
    return LoggingChannel{
        .id = std::move(id),
        .address = 0x10,
        .length = 2,
        .from_byte_expression = std::move(expression),
        .unit = "rpm",
        .decimal_precision = 2,
    };
}

fastecu::Result<LoggingSession> SessionWithPolicy(LoggingPolicy policy, std::string expression = "x")
{
    return MakeLoggingSession(LoggingProtocolId::kSsm, {Channel("rpm", std::move(expression))}, policy);
}

fastecu::Result<LoggingSession> MakeValidSession()
{
    return SessionWithPolicy({
        .poll_timeout = 100ms,
        .car_silence_miss_threshold = 3,
        .reconnect_attempt_threshold = 2,
        .reconnect_retry_period = 0,
    });
}

fastecu::Status RunUntilCancelled(const LoggingSession& session, ScriptedProtocol& protocol, RecordingLoggingSink& sink,
                                  int poll_count)
{
    FakeCancellationToken token;
    token.SetPredicate([&protocol, poll_count] { return protocol.polls_completed >= poll_count; });
    RecordingEventSink diagnostics;
    return LoggingUseCase{}.Run(session, protocol, token, sink, diagnostics);
}

} // namespace

TEST(LoggingUseCaseTest, ConvertsAndEmitsOrderedSamplesThenCancels)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"rpm", "4000"}}});
    FakeCancellationToken token;
    token.SetPredicate([&protocol] { return protocol.polls_completed >= 1; });
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;

    auto built_session_1 = MakeValidSession();
    ASSERT_THAT(built_session_1, fastecu::testing::IsOk());
    ASSERT_THAT(LoggingUseCase{}.Run(std::move(*built_session_1), protocol, token, sink, diagnostics),
                fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
    ASSERT_EQ(sink.sample_batches.size(), 1U);
    ASSERT_EQ(sink.sample_batches[0].size(), 1U);
    EXPECT_EQ(sink.sample_batches[0][0].channel_id, "rpm");
    EXPECT_EQ(sink.sample_batches[0][0].numeric_value, 4000.0);
    EXPECT_EQ(sink.states, (std::vector{LoggingState::kRunning}));
    EXPECT_EQ(protocol.poll_timeouts, (std::vector{100ms}));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, PreCancellationDoesNotStartProtocol)
{
    ScriptedProtocol protocol;
    FakeCancellationToken token(true);
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;

    auto built_session_2 = MakeValidSession();
    ASSERT_THAT(built_session_2, fastecu::testing::IsOk());
    ASSERT_THAT(LoggingUseCase{}.Run(std::move(*built_session_2), protocol, token, sink, diagnostics),
                fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
    EXPECT_EQ(protocol.starts, 0);
    EXPECT_EQ(protocol.stops, 0);
}

TEST(LoggingUseCaseTest, PreservesSilenceThresholdAndReconnectCadence)
{
    auto built_session_3 = SessionWithPolicy({
        .poll_timeout = 10ms,
        .car_silence_miss_threshold = 2,
        .reconnect_attempt_threshold = 3,
        .reconnect_retry_period = 2,
    });
    ASSERT_THAT(built_session_3, fastecu::testing::IsOk());
    auto session = std::move(*built_session_3);
    ScriptedProtocol protocol;
    protocol.polls = {
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = false},
        PollData{.responded = true, .samples = {{"rpm", "7"}}},
    };
    RecordingLoggingSink sink;

    ASSERT_THAT(RunUntilCancelled(session, protocol, sink, 4), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
    EXPECT_EQ(sink.states,
              (std::vector{LoggingState::kRunning, LoggingState::kCarNotResponding, LoggingState::kRunning}));
    EXPECT_EQ(protocol.start_call_poll_numbers, (std::vector{0, 3}));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, RetriesBadResponse)
{
    ScriptedProtocol protocol;
    protocol.polls = {
        fastecu::Fail(fastecu::ErrorKind::kBadResponse, "bad frame"),
        PollData{.responded = true, .samples = {{"rpm", "8"}}},
    };
    RecordingLoggingSink sink;

    auto built_session_4 = MakeValidSession();
    ASSERT_THAT(built_session_4, fastecu::testing::IsOk());
    ASSERT_THAT(RunUntilCancelled(std::move(*built_session_4), protocol, sink, 2),
                fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
    ASSERT_EQ(sink.sample_batches.size(), 1U);
    EXPECT_EQ(sink.sample_batches[0][0].raw_value, "8");
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, RetriesFailedReconnectAtConfiguredCadence)
{
    auto built_session_5 = SessionWithPolicy({
        .poll_timeout = 10ms,
        .car_silence_miss_threshold = 2,
        .reconnect_attempt_threshold = 3,
        .reconnect_retry_period = 2,
    });
    ASSERT_THAT(built_session_5, fastecu::testing::IsOk());
    auto session = std::move(*built_session_5);
    ScriptedProtocol protocol;
    protocol.start_results = {
        fastecu::Status{},
        fastecu::Fail(fastecu::ErrorKind::kBadResponse, "retry later"),
        fastecu::Status{},
    };
    protocol.polls = {
        PollData{.responded = false}, PollData{.responded = false}, PollData{.responded = false},
        PollData{.responded = false}, PollData{.responded = false},
    };
    RecordingLoggingSink sink;

    ASSERT_THAT(RunUntilCancelled(session, protocol, sink, 5), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
    EXPECT_EQ(protocol.start_call_poll_numbers, (std::vector{0, 3, 5}));
    EXPECT_EQ(sink.states,
              (std::vector{LoggingState::kRunning, LoggingState::kCarNotResponding, LoggingState::kRunning}));
}

class TerminalPollErrorTest : public ::testing::TestWithParam<fastecu::ErrorKind>
{
};

TEST_P(TerminalPollErrorTest, TerminatesAndCleansUpOnce)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(fastecu::Fail(GetParam(), "terminal"));
    RecordingLoggingSink sink;

    auto built_session_6 = MakeValidSession();
    ASSERT_THAT(built_session_6, fastecu::testing::IsOk());
    ASSERT_THAT(RunUntilCancelled(std::move(*built_session_6), protocol, sink, 2), fastecu::testing::IsErr(GetParam()));
    EXPECT_EQ(protocol.stops, 1);
}

INSTANTIATE_TEST_SUITE_P(LoggingUseCase, TerminalPollErrorTest,
                         ::testing::Values(fastecu::ErrorKind::kInvalidConfig, fastecu::ErrorKind::kUnsupported,
                                           fastecu::ErrorKind::kInternal, fastecu::ErrorKind::kDisconnected,
                                           fastecu::ErrorKind::kCancelled, fastecu::ErrorKind::kTimeout),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{"InvalidConfig", "Unsupported", "Internal",
                                                                    "Disconnected",  "Cancelled",   "Timeout"};
                                 return kNames[param_info.index];
                             }));

TEST(LoggingUseCaseTest, ConversionFailureContinuesHealthyPeersAndRecoversWithBoundedDiagnostics)
{
    auto built_session_7 = MakeValidSession();
    ASSERT_THAT(built_session_7, fastecu::testing::IsOk());
    const auto session =
        MakeLoggingSession(LoggingProtocolId::kSsm, {Channel("rpm", "x/(x-1)"), Channel("healthy", "x*2")},
                           std::move(*built_session_7).Policy());
    ASSERT_THAT(session, fastecu::testing::IsOk());
    ScriptedProtocol protocol;
    protocol.polls = {
        PollData{.responded = true, .samples = {{"rpm", "2"}, {"healthy", "10"}}},
        PollData{.responded = true, .samples = {{"rpm", "1"}, {"healthy", "11"}}},
        PollData{.responded = true, .samples = {{"rpm", "1"}, {"healthy", "12"}}},
        PollData{.responded = true, .samples = {{"rpm", "3"}, {"healthy", "13"}}},
    };
    FakeCancellationToken token;
    token.SetPredicate([&protocol] { return protocol.polls_completed >= 4; });
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    EXPECT_THAT(LoggingUseCase{}.Run(*session, protocol, token, sink, diagnostics),
                fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
    ASSERT_EQ(sink.sample_batches.size(), 4U);
    EXPECT_EQ(sink.sample_batches[0][0].numeric_value, 2.0);
    EXPECT_EQ(sink.sample_batches[3][0].numeric_value, 1.5);
    EXPECT_EQ(sink.sample_batches[1][0].numeric_value, std::nullopt);
    EXPECT_EQ(sink.sample_batches[2][0].numeric_value, std::nullopt);
    EXPECT_EQ(sink.sample_batches[1][1].numeric_value, 22.0);
    EXPECT_EQ(sink.sample_batches[2][1].numeric_value, 24.0);
    EXPECT_EQ(sink.sample_batches[3][1].numeric_value, 26.0);
    ASSERT_EQ(diagnostics.logs.size(), 1U);
    EXPECT_THAT(diagnostics.logs[0].second, ::testing::HasSubstr("rpm"));
    EXPECT_EQ(protocol.stops, 1);
}

TEST(LoggingUseCaseTest, UnknownProtocolChannelTerminatesAsInternal)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(PollData{.responded = true, .samples = {{"rpm", "2"}, {"unknown", "1"}}});
    RecordingLoggingSink sink;

    auto built_session_8 = MakeValidSession();
    ASSERT_THAT(built_session_8, fastecu::testing::IsOk());
    ASSERT_THAT(RunUntilCancelled(std::move(*built_session_8), protocol, sink, 2),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_EQ(protocol.stops, 1);
    EXPECT_THAT(sink.sample_batches, IsEmpty());
}

TEST(LoggingUseCaseTest, PrimaryErrorWinsOverStopFailure)
{
    ScriptedProtocol protocol;
    protocol.polls.push_back(fastecu::Fail(fastecu::ErrorKind::kDisconnected, "lost"));
    protocol.stop_result = fastecu::Fail(fastecu::ErrorKind::kInternal, "cleanup");
    RecordingLoggingSink sink;
    RecordingEventSink diagnostics;
    FakeCancellationToken token;
    token.SetPredicate([&protocol] { return protocol.polls_completed >= 2; });

    auto built_session_9 = MakeValidSession();
    ASSERT_THAT(built_session_9, fastecu::testing::IsOk());
    ASSERT_THAT(LoggingUseCase{}.Run(std::move(*built_session_9), protocol, token, sink, diagnostics),
                fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
    EXPECT_EQ(protocol.stops, 1);
    ASSERT_EQ(diagnostics.logs.size(), 1U);
    EXPECT_EQ(diagnostics.logs[0].first, fastecu::LogLevel::kError);
}
