#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>
#include <QElapsedTimer>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"

#include <chrono>

#include "src/backend/logging/testing/scripted_logging_protocol.h"
#include "src/platform/desktop/common/logging/runtime/logging_worker.h"

namespace fastecu::desktop::logging
{

namespace
{

using namespace fastecu::logging;
using namespace std::chrono_literals;

class NullDiagnostics final : public fastecu::IEventSink
{
  public:
    void log(fastecu::LogLevel, std::string_view) override
    {
    }
    void progress(int, int) override
    {
    }
    void notice(std::string_view) override
    {
    }
};

LoggingSession session(LoggingPolicy policy = {.poll_timeout = 5ms,
                                               .car_silence_miss_threshold = 2,
                                               .reconnect_attempt_threshold = 1000,
                                               .reconnect_retry_period = 0})
{
    auto result = make_logging_session(LoggingProtocolId::Ssm,
                                       {LoggingChannel{.id = "rpm",
                                                       .address = 0x10,
                                                       .length = 1,
                                                       .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                                                       .from_byte_expression = "x",
                                                       .unit = "rpm",
                                                       .decimal_precision = 0}},
                                       policy);
    Q_ASSERT(result.has_value());
    return std::move(*result);
}

} // namespace

TEST(TestLoggingWorker, forwards_portable_states_samples_and_cancelled_result)
{
    ScriptedLoggingProtocol protocol;
    protocol.queueStartResult({});
    protocol.queuePollResult(PollData{.responded = false});
    protocol.queuePollResult(PollData{.responded = false});
    protocol.queuePollResult(
        PollData{.responded = true, .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "1234"}}});
    NullDiagnostics diagnostics;
    LoggingWorker worker(session(), &protocol, diagnostics);
    fastecu::testing::SignalRecorder state_spy(&worker, &LoggingWorker::stateChanged);
    fastecu::testing::SignalRecorder samples_spy(&worker, &LoggingWorker::samplesReady);
    fastecu::testing::SignalRecorder finished_spy(&worker, &LoggingWorker::sessionFinished);

    worker.start();
    // Cue off the protocol fake's own condition variable, then join. A
    // signal recorder connects with Qt::DirectConnection, so the worker thread
    // records samplesReady itself; signal recorder::wait() is edge-triggered
    // and reports only emissions arriving after it snapshots its baseline
    // count, so an emission that lands first makes it burn its whole
    // timeout and return false. Joining is what makes the spies final.
    ASSERT_TRUE(protocol.waitUntilQueuedPollResultsConsumed(std::chrono::milliseconds(2000)));
    worker.requestStop();
    ASSERT_TRUE(worker.wait(2000));

    ASSERT_TRUE(state_spy.count() >= 3);
    ASSERT_EQ(std::get<0>(state_spy.snapshot().at(0)), LoggingState::Running);
    ASSERT_EQ(std::get<0>(state_spy.snapshot().at(1)), LoggingState::CarNotResponding);
    ASSERT_EQ(std::get<0>(state_spy.snapshot().at(2)), LoggingState::Running);
    const auto samples = std::get<0>(samples_spy.snapshot().at(0));
    ASSERT_EQ(samples.size(), 1U);
    ASSERT_EQ(samples.at(0).channel_id, std::string("rpm"));
    ASSERT_EQ(samples.at(0).numeric_value, 1234.0);
    ASSERT_EQ(finished_spy.count(), 1U);
    const auto result = std::get<0>(finished_spy.snapshot().at(0));
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
    ASSERT_TRUE(protocol.stopCalled());
}

TEST(TestLoggingWorker, forwards_final_start_error_without_policy_mapping)
{
    ScriptedLoggingProtocol protocol;
    protocol.queueStartResult(fastecu::fail(fastecu::ErrorKind::BadResponse, "handshake rejected"));
    NullDiagnostics diagnostics;
    LoggingWorker worker(session(), &protocol, diagnostics);
    fastecu::testing::SignalRecorder finished_spy(&worker, &LoggingWorker::sessionFinished);

    worker.start();
    // Joining is sufficient: run() emits sessionFinished last, so once the
    // thread is joined the spy is final. See the note above for why
    // signal recorder::wait() is the wrong tool here.
    ASSERT_TRUE(worker.wait(2000));

    const auto result = std::get<0>(finished_spy.snapshot().at(0));
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
    ASSERT_EQ(result.error().detail, std::string("handshake rejected"));
}

TEST(TestLoggingWorker, destruction_cancels_and_joins_a_blocked_poll)
{
    ScriptedLoggingProtocol protocol;
    protocol.queueStartResult({});
    protocol.blockPollUntilCancelled();
    NullDiagnostics diagnostics;
    QElapsedTimer elapsed;
    elapsed.start();
    {
        LoggingWorker worker(session(), &protocol, diagnostics);
        worker.start();
        ASSERT_TRUE(protocol.waitUntilPollEntered(std::chrono::milliseconds(500)));
    }

    ASSERT_TRUE(elapsed.elapsed() < 500) << "worker destruction exceeded cancellation bound";
    ASSERT_TRUE(protocol.stopCalled());
}

} // namespace fastecu::desktop::logging

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
