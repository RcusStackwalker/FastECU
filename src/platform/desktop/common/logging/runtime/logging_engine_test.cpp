#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <string>
#include <gtest/gtest.h>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include "src/backend/logging/testing/scripted_logging_protocol.h"
#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"

namespace fastecu::desktop::logging
{

namespace
{

using fastecu::desktop::logging::DesktopLoggingSnapshot;
using namespace fastecu::logging;
using namespace std::chrono_literals;

DesktopLoggingSnapshot snapshot()
{
    auto session = make_logging_session(LoggingProtocolId::Ssm,
                                        {LoggingChannel{.id = "rpm",
                                                        .address = 0x10,
                                                        .length = 1,
                                                        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                                                        .from_byte_expression = "x",
                                                        .unit = "rpm",
                                                        .decimal_precision = 0}},
                                        LoggingPolicy{.poll_timeout = 5ms,
                                                      .car_silence_miss_threshold = 2,
                                                      .reconnect_attempt_threshold = 1000,
                                                      .reconnect_retry_period = 0});
    Q_ASSERT(session.has_value());
    return DesktopLoggingSnapshot{.session = std::move(*session),
                                  .protocol = "SSM",
                                  .identities_by_id = {{"rpm", {"SSM", "rpm"}}},
                                  .enabled_ids = {"rpm"}};
}

class BlockingFailureProtocol final : public fastecu::logging::LoggingProtocol
{
  public:
    explicit BlockingFailureProtocol(std::atomic<int> *stop_calls = nullptr) : stop_calls_(stop_calls)
    {
    }

    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        return {};
    }

    fastecu::Result<fastecu::logging::PollData> poll(std::chrono::milliseconds,
                                                     const fastecu::ICancellationToken& cancellation) override
    {
        std::unique_lock lock(mutex_);
        poll_entered_ = true;
        poll_entered_cv_.notify_all();
        while (!released_ && !cancellation.cancelled())
        {
            release_cv_.wait_for(lock, std::chrono::milliseconds(1));
        }
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "active run cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, "active run failed");
    }

    fastecu::Status stop() override
    {
        if (stop_calls_)
        {
            stop_calls_->fetch_add(1, std::memory_order_relaxed);
        }
        return {};
    }

    bool waitUntilPollEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return poll_entered_cv_.wait_for(lock, timeout, [this] { return poll_entered_; });
    }

    void releaseFailure()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        release_cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable poll_entered_cv_;
    std::condition_variable release_cv_;
    bool poll_entered_ = false;
    bool released_ = false;
    std::atomic<int> *stop_calls_;
};

class SampleThenBlockProtocol final : public fastecu::logging::LoggingProtocol
{
  public:
    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        return {};
    }

    fastecu::Result<fastecu::logging::PollData> poll(std::chrono::milliseconds,
                                                     const fastecu::ICancellationToken& cancellation) override
    {
        std::unique_lock lock(mutex_);
        if (!sample_returned_)
        {
            sample_returned_ = true;
            return PollData{.responded = true, .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "42"}}};
        }

        blocking_poll_entered_ = true;
        blocking_poll_entered_cv_.notify_all();
        while (!cancellation.cancelled())
        {
            cancellation_cv_.wait_for(lock, std::chrono::milliseconds(1));
        }
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "first run cancelled");
    }

    fastecu::Status stop() override
    {
        return {};
    }

    bool waitUntilBlockingPollEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_poll_entered_cv_.wait_for(lock, timeout, [this] { return blocking_poll_entered_; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable blocking_poll_entered_cv_;
    std::condition_variable cancellation_cv_;
    bool sample_returned_ = false;
    bool blocking_poll_entered_ = false;
};

void expect_start_error(fastecu::Status result, fastecu::ErrorKind kind, std::string_view detail)
{
    ASSERT_TRUE(!result);
    ASSERT_EQ(result.error().kind, kind);
    ASSERT_EQ(result.error().detail, std::string(detail));
}

} // namespace

struct StartRejectionsCase
{
    std::string name;
    int source;
    int kind;
    QString detail;
};
class StartRejectionsParameters : public ::testing::Test, public ::testing::WithParamInterface<StartRejectionsCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, StartRejectionsParameters,
    ::testing::Values(StartRejectionsCase{"active_run", 0, static_cast<int>(fastecu::ErrorKind::InvalidConfig),
                                          QString("a logging run is already active")},
                      StartRejectionsCase{"unknown_ID", 1, static_cast<int>(fastecu::ErrorKind::InvalidConfig),
                                          QString("no logging protocol registered for 'NOPE'")},
                      StartRejectionsCase{"null_factory_value", 2, static_cast<int>(fastecu::ErrorKind::Internal),
                                          QString("protocol factory for 'TEST' returned null")},
                      StartRejectionsCase{"returned_error", 3, static_cast<int>(fastecu::ErrorKind::Disconnected),
                                          QString("open failed")},
                      StartRejectionsCase{"std_exception", 4, static_cast<int>(fastecu::ErrorKind::Internal),
                                          QString("driver setup exploded")},
                      StartRejectionsCase{"unknown_exception", 5, static_cast<int>(fastecu::ErrorKind::Internal),
                                          QString("protocol factory threw an unknown exception")}),
    [](const ::testing::TestParamInfo<StartRejectionsCase>& info) { return info.param.name; });

TEST_P(StartRejectionsParameters, start_rejections)
{
    const int source = GetParam().source;
    const int kind = GetParam().kind;
    const QString detail = GetParam().detail;

    LoggingEngine engine;
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);
    fastecu::testing::SignalRecorder error_spy(&engine, &LoggingEngine::LOG_E);

    BlockingFailureProtocol *active_protocol = nullptr;
    if (source == 0)
    {
        active_protocol = new BlockingFailureProtocol();
        engine.registerProtocol("TEST", [active_protocol](const DesktopLoggingSnapshot&)
                                { return std::unique_ptr<LoggingProtocol>(active_protocol); });
        ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
        ASSERT_TRUE(active_protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
    }
    else if (source == 2)
    {
        engine.registerProtocol("TEST",
                                [](const DesktopLoggingSnapshot&) { return std::unique_ptr<LoggingProtocol>(); });
    }
    else if (source == 3)
    {
        engine.registerProtocol("TEST",
                                [](const DesktopLoggingSnapshot&) -> fastecu::Result<std::unique_ptr<LoggingProtocol>>
                                { return fastecu::fail(fastecu::ErrorKind::Disconnected, "open failed"); });
    }
    else if (source == 4)
    {
        engine.registerProtocol("TEST", [](const DesktopLoggingSnapshot&) -> std::unique_ptr<LoggingProtocol>
                                { throw std::runtime_error("driver setup exploded"); });
    }
    else if (source == 5)
    {
        engine.registerProtocol("TEST",
                                [](const DesktopLoggingSnapshot&) -> std::unique_ptr<LoggingProtocol>
                                {
                                    // Exercises the catch-all branch for a throw not derived from
                                    // std::exception.
                                    // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
                                    throw 42;
                                });
    }

    const auto result = engine.start(LogSessionConfig{.protocol_id = source == 1 ? "NOPE" : "TEST"}, snapshot());
    ASSERT_NO_FATAL_FAILURE(expect_start_error(result, static_cast<fastecu::ErrorKind>(kind), detail.toStdString()));
    ASSERT_EQ(ended_spy.count(), 0U);
    ASSERT_EQ(error_spy.count(), 1U);

    if (source != 0)
    {
        ASSERT_TRUE(!engine.isRunning());
        return;
    }

    ASSERT_TRUE(engine.isRunning());
    active_protocol->releaseFailure();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));
    ASSERT_EQ(ended_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::RuntimeFailed);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(0)), QString("active run failed"));
    ASSERT_EQ(error_spy.count(), 2U);
    ASSERT_TRUE(!engine.isRunning());
}

// signal recorder::wait() is safe in this suite -- unlike in logging_worker_test.cpp
// -- because LoggingEngine does not emit on the worker thread. It receives
// LoggingWorker's signals over a queued connection (worker and engine live on
// different threads), so sessionEnded/valuesUpdated are re-emitted on this
// thread from inside the very event loop wait() is running. The emission
// therefore cannot precede wait()'s baseline snapshot the way it can when a
// spy is attached straight to a QThread subclass's own signal.
TEST(TestLoggingEngine, user_stop_publishes_joined_completion_exactly_once)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->blockPollUntilCancelled();
    bool saw_session = false;
    engine.registerProtocol("TEST",
                            [protocol, &saw_session](const DesktopLoggingSnapshot& value)
                            {
                                saw_session = value.session.find_channel("rpm") != nullptr;
                                return std::unique_ptr<LoggingProtocol>(protocol);
                            });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);
    fastecu::testing::SignalRecorder error_spy(&engine, &LoggingEngine::LOG_E);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
    ASSERT_TRUE(saw_session);
    ASSERT_TRUE(engine.isRunning());

    engine.stop();
    ASSERT_EQ(ended_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::StoppedByUser);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(0)), QString());
    ASSERT_TRUE(!engine.isRunning());
    engine.stop();
    ASSERT_EQ(ended_spy.count(), 1U);
    ASSERT_EQ(error_spy.count(), 0U);
}

TEST(TestLoggingEngine, completion_observer_can_immediately_start_a_second_run)
{
    LoggingEngine engine;
    auto *first_protocol = new ScriptedLoggingProtocol();
    first_protocol->queueStartResult({});
    first_protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Internal, "first run failed"));
    auto *second_protocol = new ScriptedLoggingProtocol();
    second_protocol->queueStartResult({});
    second_protocol->blockPollUntilCancelled();
    int factory_calls = 0;
    engine.registerProtocol("TEST",
                            [first_protocol, second_protocol, &factory_calls](const DesktopLoggingSnapshot&)
                            {
                                ++factory_calls;
                                return std::unique_ptr<LoggingProtocol>(factory_calls == 1 ? first_protocol
                                                                                           : second_protocol);
                            });

    bool restart_attempted = false;
    bool observer_saw_idle = false;
    std::optional<fastecu::Status> restart_result;
    QObject::connect(&engine, &LoggingEngine::sessionEnded, &engine,
                     [&](SessionEndReason, const QString&)
                     {
                         if (restart_attempted)
                         {
                             return;
                         }
                         restart_attempted = true;
                         observer_saw_idle = !engine.isRunning();
                         restart_result.emplace(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
                     });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));

    ASSERT_TRUE(restart_attempted);
    ASSERT_TRUE(observer_saw_idle);
    ASSERT_TRUE(restart_result.has_value());
    ASSERT_TRUE(*restart_result);
    ASSERT_TRUE(second_protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
    ASSERT_TRUE(engine.isRunning());
    engine.stop();
}

TEST(TestLoggingEngine, explicit_stop_restart_ignores_stale_worker_events_and_preserves_handshake_classification)
{
    LoggingEngine engine;
    auto *first_protocol = new SampleThenBlockProtocol();
    auto *second_protocol = new ScriptedLoggingProtocol();
    second_protocol->queueStartResult(fastecu::fail(fastecu::ErrorKind::BadResponse, "second handshake failed"));
    int factory_calls = 0;
    engine.registerProtocol("TEST",
                            [first_protocol, second_protocol, &factory_calls](const DesktopLoggingSnapshot&)
                            {
                                ++factory_calls;
                                if (factory_calls == 1)
                                {
                                    return std::unique_ptr<LoggingProtocol>(first_protocol);
                                }
                                return std::unique_ptr<LoggingProtocol>(second_protocol);
                            });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);
    fastecu::testing::SignalRecorder status_spy(&engine, &LoggingEngine::statusChanged);
    fastecu::testing::SignalRecorder value_spy(&engine, &LoggingEngine::valuesUpdated);
    bool restart_succeeded = false;
    QObject::connect(&engine, &LoggingEngine::sessionEnded, &engine,
                     [&](SessionEndReason reason, const QString&)
                     {
                         if (reason == SessionEndReason::StoppedByUser)
                         {
                             restart_succeeded =
                                 engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()).has_value();
                         }
                     });

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(first_protocol->waitUntilBlockingPollEntered(std::chrono::milliseconds(500)));
    engine.stop();
    ASSERT_TRUE(restart_succeeded);

    ASSERT_TRUE(
        fastecu::testing::wait_until([&] { return (ended_spy.count()) == (2); }, std::chrono::milliseconds(2000)));
    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::StoppedByUser);
    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(1)), SessionEndReason::HandshakeFailed);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(1)), QString("second handshake failed"));
    ASSERT_EQ(status_spy.count(), 0U);
    ASSERT_EQ(value_spy.count(), 0U);
    ASSERT_TRUE(!engine.isRunning());
}

TEST(TestLoggingEngine, natural_terminal_result_is_published_once_after_reprocessing_queued_delivery)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Internal, "terminal failure"));
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));
    ASSERT_EQ(ended_spy.count(), 1U);

    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(ended_spy.count(), 1U);
    ASSERT_TRUE(!engine.isRunning());
}

TEST(TestLoggingEngine, successful_worker_result_is_reported_as_runtime_failure)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->blockPollUntilCancelled();
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "handleWorkerSessionFinished", Qt::DirectConnection,
                                          Q_ARG(fastecu::Status, fastecu::Status{})));

    ASSERT_EQ(ended_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::RuntimeFailed);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(0)), QString("logging run ended without an error"));
    ASSERT_TRUE(!engine.isRunning());
}

TEST(TestLoggingEngine, destruction_joins_blocked_run_without_publishing_completion)
{
    std::atomic<int> protocol_stop_calls{0};
    int completion_count = 0;
    auto *engine = new LoggingEngine();
    auto *protocol = new BlockingFailureProtocol(&protocol_stop_calls);
    engine->registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                             { return std::unique_ptr<LoggingProtocol>(protocol); });
    QObject::connect(engine, &LoggingEngine::sessionEnded,
                     [&completion_count](SessionEndReason, const QString&) { ++completion_count; });

    ASSERT_TRUE(engine->start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
    delete engine;

    ASSERT_EQ(protocol_stop_calls.load(std::memory_order_relaxed), 1);
    ASSERT_EQ(completion_count, 0);
}

TEST(TestLoggingEngine, every_cdbg_serial_setup_failure_is_structured_and_stops_before_later_steps)
{
    using fastecu::desktop::logging::CdbgSerialSetupActions;
    using fastecu::desktop::logging::configure_cdbg_serial;

    for (int failure = 0; failure < 7; ++failure)
    {
        int calls = 0;
        const auto step = [&calls, failure]() { return calls++ != failure; };
        const auto status = configure_cdbg_serial(CdbgSerialSetupActions{
            .disable_iso14230 = step,
            .disable_iso14230_header = step,
            .enable_raw_can = step,
            .disable_iso15765 = step,
            .select_11_bit_ids = step,
            .select_500k_baud = step,
            .select_reply_id = step,
        });

        ASSERT_TRUE(!status);
        ASSERT_EQ(status.error().kind, fastecu::ErrorKind::InvalidConfig);
        ASSERT_EQ(calls, failure + 1);
    }
}

TEST(TestLoggingEngine, start_error_preserves_handshake_failure_ui_path)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult(fastecu::fail(fastecu::ErrorKind::BadResponse, "no ECU"));
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);
    fastecu::testing::SignalRecorder error_spy(&engine, &LoggingEngine::LOG_E);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));

    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::HandshakeFailed);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(0)), QString("no ECU"));
    ASSERT_EQ(std::get<0>(error_spy.snapshot().at(0)), QString("Logging session failed to start: no ECU"));
    ASSERT_TRUE(!engine.isRunning());
}

TEST(TestLoggingEngine, disconnect_error_preserves_adapter_failure_ui_path)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Disconnected, "port closed"));
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));

    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::AdapterDisconnected);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(0)), QString("port closed"));
    ASSERT_TRUE(!engine.isRunning());
}

TEST(TestLoggingEngine, post_start_failure_is_not_reported_as_handshake_failure)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Internal, "bad stream frame"));
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);
    fastecu::testing::SignalRecorder error_spy(&engine, &LoggingEngine::LOG_E);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));

    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::RuntimeFailed);
    ASSERT_EQ(std::get<0>(error_spy.snapshot().at(0)), QString("Logging session failed: bad stream frame"));
}

TEST(TestLoggingEngine, unexpected_cancelled_outcome_is_reported_as_runtime_failure)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted poll cancelled"));
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder ended_spy(&engine, &LoggingEngine::sessionEnded);
    fastecu::testing::SignalRecorder error_spy(&engine, &LoggingEngine::LOG_E);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return ended_spy.count() != 0; }, std::chrono::milliseconds(2000)));

    ASSERT_EQ(ended_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(ended_spy.snapshot().at(0)), SessionEndReason::RuntimeFailed);
    ASSERT_EQ(std::get<1>(ended_spy.snapshot().at(0)), QString("scripted poll cancelled"));
    ASSERT_EQ(error_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(error_spy.snapshot().at(0)), QString("Logging session failed: scripted poll cancelled"));
    ASSERT_TRUE(!engine.isRunning());
}

TEST(TestLoggingEngine, diagnostic_slot_forwards_error_level_with_timestamp_and_linefeed)
{
    LoggingEngine engine;
    fastecu::testing::SignalRecorder error_spy(&engine, &LoggingEngine::LOG_E);

    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Error)),
                                          Q_ARG(QString, QString("error diagnostic"))));

    ASSERT_EQ(error_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(error_spy.snapshot().at(0)), QString("error diagnostic"));
    ASSERT_EQ(std::get<1>(error_spy.snapshot().at(0)), true);
    ASSERT_EQ(std::get<2>(error_spy.snapshot().at(0)), true);
}

TEST(TestLoggingEngine, diagnostic_slot_forwards_warning_level_with_timestamp_and_linefeed)
{
    LoggingEngine engine;
    fastecu::testing::SignalRecorder warning_spy(&engine, &LoggingEngine::LOG_W);

    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Warning)),
                                          Q_ARG(QString, QString("warning diagnostic"))));

    ASSERT_EQ(warning_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(warning_spy.snapshot().at(0)), QString("warning diagnostic"));
    ASSERT_EQ(std::get<1>(warning_spy.snapshot().at(0)), true);
    ASSERT_EQ(std::get<2>(warning_spy.snapshot().at(0)), true);
}

TEST(TestLoggingEngine, diagnostic_slot_forwards_info_level_with_timestamp_and_linefeed)
{
    LoggingEngine engine;
    fastecu::testing::SignalRecorder info_spy(&engine, &LoggingEngine::LOG_I);

    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Info)),
                                          Q_ARG(QString, QString("info diagnostic"))));

    ASSERT_EQ(info_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(info_spy.snapshot().at(0)), QString("info diagnostic"));
    ASSERT_EQ(std::get<1>(info_spy.snapshot().at(0)), true);
    ASSERT_EQ(std::get<2>(info_spy.snapshot().at(0)), true);
}

TEST(TestLoggingEngine, diagnostic_slot_forwards_debug_level_with_timestamp_and_linefeed)
{
    LoggingEngine engine;
    fastecu::testing::SignalRecorder debug_spy(&engine, &LoggingEngine::LOG_D);

    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Debug)),
                                          Q_ARG(QString, QString("debug diagnostic"))));

    ASSERT_EQ(debug_spy.count(), 1U);
    ASSERT_EQ(std::get<0>(debug_spy.snapshot().at(0)), QString("debug diagnostic"));
    ASSERT_EQ(std::get<1>(debug_spy.snapshot().at(0)), true);
    ASSERT_EQ(std::get<2>(debug_spy.snapshot().at(0)), true);
}

TEST(TestLoggingEngine, portable_events_map_to_existing_status_and_value_signals)
{
    LoggingEngine engine;
    auto *protocol = new ScriptedLoggingProtocol();
    protocol->queueStartResult({});
    protocol->queuePollResult(PollData{.responded = false});
    protocol->queuePollResult(PollData{.responded = false});
    protocol->queuePollResult(
        PollData{.responded = true, .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "42"}}});
    engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                            { return std::unique_ptr<LoggingProtocol>(protocol); });
    fastecu::testing::SignalRecorder status_spy(&engine, &LoggingEngine::statusChanged);
    fastecu::testing::SignalRecorder value_spy(&engine, &LoggingEngine::valuesUpdated);

    ASSERT_TRUE(engine.start(LogSessionConfig{.protocol_id = "TEST"}, snapshot()));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return value_spy.count() != 0; }, std::chrono::milliseconds(2000)));
    engine.stop();

    ASSERT_TRUE(status_spy.count() >= 3);
    ASSERT_EQ(std::get<0>(status_spy.snapshot().at(0)), LoggingStatus::Running);
    ASSERT_EQ(std::get<0>(status_spy.snapshot().at(1)), LoggingStatus::CarNotResponding);
    ASSERT_EQ(std::get<0>(status_spy.snapshot().at(2)), LoggingStatus::Running);
    const auto values = std::get<0>(value_spy.snapshot().at(0));
    ASSERT_EQ(values.at(0).channel_id, std::string("rpm"));
    ASSERT_EQ(values.at(0).numeric_value, 42.0);
}

} // namespace fastecu::desktop::logging

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
