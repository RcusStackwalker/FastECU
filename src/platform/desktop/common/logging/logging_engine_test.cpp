#include <QtTest>
#include <QSignalSpy>

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include "src/backend/logging/testing/scripted_logging_protocol.h"
#include "src/platform/desktop/common/logging/logging_engine.h"

namespace fastecu::desktop::logging
{

namespace
{

using namespace fastecu::logging;

static_assert(!std::copy_constructible<LoggingRun>);
static_assert(std::move_constructible<LoggingRun>);

LoggingSession session()
{
    auto result = make_logging_session(LoggingProtocolId::Ssm,
                                       {LoggingChannel{.id = "rpm",
                                                       .address = 0x10,
                                                       .length = 1,
                                                       .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                                                       .from_byte_expression = "x",
                                                       .unit = "rpm",
                                                       .decimal_precision = 0}},
                                       LoggingPolicy{.poll_timeout_ms = 5,
                                                     .car_silence_miss_threshold = 2,
                                                     .reconnect_attempt_threshold = 1000,
                                                     .reconnect_retry_period = 0});
    Q_ASSERT(result.has_value());
    return std::move(*result);
}

LoggingRun run(std::unique_ptr<LoggingProtocol> protocol)
{
    return {.session = session(), .protocol = std::move(protocol)};
}

class BlockingFailureProtocol final : public fastecu::logging::LoggingProtocol
{
  public:
    explicit BlockingFailureProtocol(std::atomic<int> *stop_calls = nullptr,
                                     std::atomic<int> *destruction_count = nullptr)
        : stop_calls_(stop_calls), destruction_count_(destruction_count)
    {
    }

    ~BlockingFailureProtocol() override
    {
        if (destruction_count_)
        {
            destruction_count_->fetch_add(1, std::memory_order_relaxed);
        }
    }

    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        return {};
    }

    fastecu::Result<fastecu::logging::PollData> poll(int, const fastecu::ICancellationToken& cancellation) override
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
    std::atomic<int> *destruction_count_;
};

class SampleThenBlockProtocol final : public fastecu::logging::LoggingProtocol
{
  public:
    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        return {};
    }

    fastecu::Result<fastecu::logging::PollData> poll(int, const fastecu::ICancellationToken& cancellation) override
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
    QVERIFY(!result);
    QCOMPARE(result.error().kind, kind);
    QCOMPARE(result.error().detail, std::string(detail));
}

} // namespace

// QSignalSpy::wait() is safe in this suite -- unlike in logging_worker_test.cpp
// -- because LoggingEngine does not emit on the worker thread. It receives
// LoggingWorker's signals over a queued connection (worker and engine live on
// different threads), so sessionEnded/valuesUpdated are re-emitted on this
// thread from inside the very event loop wait() is running. The emission
// therefore cannot precede wait()'s baseline snapshot the way it can when a
// spy is attached straight to a QThread subclass's own signal.
class TestLoggingEngine : public QObject
{
    Q_OBJECT
  private slots:
    void start_rejections_data()
    {
        QTest::addColumn<bool>("active_run");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("detail");

        QTest::newRow("active run") << true << static_cast<int>(fastecu::ErrorKind::InvalidConfig)
                                    << QString("a logging run is already active");
        QTest::newRow("null protocol") << false << static_cast<int>(fastecu::ErrorKind::InvalidConfig)
                                       << QString("logging protocol is null");
    }

    void start_rejections()
    {
        QFETCH(bool, active_run);
        QFETCH(int, kind);
        QFETCH(QString, detail);

        LoggingEngine engine;
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        BlockingFailureProtocol *active_protocol = nullptr;
        if (active_run)
        {
            active_protocol = new BlockingFailureProtocol();
            QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(active_protocol))));
            QVERIFY(active_protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        }

        std::unique_ptr<LoggingProtocol> rejected_protocol;
        if (active_run)
        {
            rejected_protocol = std::make_unique<ScriptedLoggingProtocol>();
        }
        const auto result = engine.start(run(std::move(rejected_protocol)));
        expect_start_error(result, static_cast<fastecu::ErrorKind>(kind), detail.toStdString());
        QCOMPARE(ended_spy.size(), 0);
        QCOMPARE(error_spy.size(), 1);

        if (!active_run)
        {
            QVERIFY(!engine.isRunning());
            return;
        }

        QVERIFY(engine.isRunning());
        active_protocol->releaseFailure();
        QVERIFY(ended_spy.wait(2000));
        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::RuntimeFailed);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("active run failed"));
        QCOMPARE(error_spy.size(), 2);
        QVERIFY(!engine.isRunning());
    }

    void user_stop_publishes_joined_completion_exactly_once()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->blockPollUntilCancelled();
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        QVERIFY(engine.isRunning());

        engine.stop();
        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::StoppedByUser);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString());
        QVERIFY(!engine.isRunning());
        engine.stop();
        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(error_spy.size(), 0);
    }

    void owned_protocol_lives_through_poll_and_is_destroyed_before_joined_completion()
    {
        std::atomic<int> protocol_stop_calls{0};
        std::atomic<int> protocol_destructions{0};
        LoggingEngine engine;
        auto *protocol = new BlockingFailureProtocol(&protocol_stop_calls, &protocol_destructions);
        int destructions_observed_at_completion = -1;
        connect(&engine, &LoggingEngine::sessionEnded,
                [&protocol_destructions, &destructions_observed_at_completion](SessionEndReason, const QString&)
                { destructions_observed_at_completion = protocol_destructions.load(std::memory_order_relaxed); });

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        QCOMPARE(protocol_destructions.load(std::memory_order_relaxed), 0);

        engine.stop();

        QCOMPARE(protocol_stop_calls.load(std::memory_order_relaxed), 1);
        QCOMPARE(protocol_destructions.load(std::memory_order_relaxed), 1);
        QCOMPARE(destructions_observed_at_completion, 1);
    }

    void completion_observer_can_immediately_start_a_second_run()
    {
        LoggingEngine engine;
        auto *first_protocol = new ScriptedLoggingProtocol();
        first_protocol->queueStartResult({});
        first_protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Internal, "first run failed"));
        auto *second_protocol = new ScriptedLoggingProtocol();
        second_protocol->queueStartResult({});
        second_protocol->blockPollUntilCancelled();

        bool restart_attempted = false;
        bool observer_saw_idle = false;
        std::optional<fastecu::Status> restart_result;
        connect(&engine, &LoggingEngine::sessionEnded, &engine,
                [&](SessionEndReason, const QString&)
                {
                    if (restart_attempted)
                    {
                        return;
                    }
                    restart_attempted = true;
                    observer_saw_idle = !engine.isRunning();
                    restart_result.emplace(engine.start(run(std::unique_ptr<LoggingProtocol>(second_protocol))));
                });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(first_protocol))));
        QVERIFY(ended_spy.wait(2000));

        QVERIFY(restart_attempted);
        QVERIFY(observer_saw_idle);
        QVERIFY(restart_result.has_value());
        QVERIFY(*restart_result);
        QVERIFY(second_protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        QVERIFY(engine.isRunning());
        engine.stop();
    }

    void explicit_stop_restart_ignores_stale_worker_events_and_preserves_handshake_classification()
    {
        LoggingEngine engine;
        auto *first_protocol = new SampleThenBlockProtocol();
        auto *second_protocol = new ScriptedLoggingProtocol();
        second_protocol->queueStartResult(fastecu::fail(fastecu::ErrorKind::BadResponse, "second handshake failed"));
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy status_spy(&engine, &LoggingEngine::statusChanged);
        QSignalSpy value_spy(&engine, &LoggingEngine::valuesUpdated);
        bool restart_succeeded = false;
        connect(&engine, &LoggingEngine::sessionEnded, &engine,
                [&](SessionEndReason reason, const QString&)
                {
                    if (reason == SessionEndReason::StoppedByUser)
                    {
                        restart_succeeded =
                            engine.start(run(std::unique_ptr<LoggingProtocol>(second_protocol))).has_value();
                    }
                });

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(first_protocol))));
        QVERIFY(first_protocol->waitUntilBlockingPollEntered(std::chrono::milliseconds(500)));
        engine.stop();
        QVERIFY(restart_succeeded);

        QTRY_COMPARE_WITH_TIMEOUT(ended_spy.size(), 2, 2000);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::StoppedByUser);
        QCOMPARE(ended_spy.at(1).at(0).value<SessionEndReason>(), SessionEndReason::HandshakeFailed);
        QCOMPARE(ended_spy.at(1).at(1).toString(), QString("second handshake failed"));
        QCOMPARE(status_spy.size(), 0);
        QCOMPARE(value_spy.size(), 0);
        QVERIFY(!engine.isRunning());
    }

    void natural_terminal_result_is_published_once_after_reprocessing_queued_delivery()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Internal, "terminal failure"));
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(ended_spy.wait(2000));
        QCOMPARE(ended_spy.size(), 1);

        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCOMPARE(ended_spy.size(), 1);
        QVERIFY(!engine.isRunning());
    }

    void successful_worker_result_is_reported_as_runtime_failure()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->blockPollUntilCancelled();
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        QVERIFY(QMetaObject::invokeMethod(&engine, "handleWorkerSessionFinished", Qt::DirectConnection,
                                          Q_ARG(fastecu::Status, fastecu::Status{})));

        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::RuntimeFailed);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("logging run ended without an error"));
        QVERIFY(!engine.isRunning());
    }

    void destruction_joins_blocked_run_without_publishing_completion()
    {
        std::atomic<int> protocol_stop_calls{0};
        int completion_count = 0;
        auto *engine = new LoggingEngine();
        auto *protocol = new BlockingFailureProtocol(&protocol_stop_calls);
        connect(engine, &LoggingEngine::sessionEnded,
                [&completion_count](SessionEndReason, const QString&) { ++completion_count; });

        QVERIFY(engine->start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        delete engine;

        QCOMPARE(protocol_stop_calls.load(std::memory_order_relaxed), 1);
        QCOMPARE(completion_count, 0);
    }

    void start_error_preserves_handshake_failure_ui_path()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult(fastecu::fail(fastecu::ErrorKind::BadResponse, "no ECU"));
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::HandshakeFailed);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("no ECU"));
        QCOMPARE(error_spy.at(0).at(0).toString(), QString("Logging session failed to start: no ECU"));
        QVERIFY(!engine.isRunning());
    }

    void disconnect_error_preserves_adapter_failure_ui_path()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Disconnected, "port closed"));
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::AdapterDisconnected);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("port closed"));
        QVERIFY(!engine.isRunning());
    }

    void post_start_failure_is_not_reported_as_handshake_failure()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Internal, "bad stream frame"));
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::RuntimeFailed);
        QCOMPARE(error_spy.at(0).at(0).toString(), QString("Logging session failed: bad stream frame"));
    }

    void unexpected_cancelled_outcome_is_reported_as_runtime_failure()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted poll cancelled"));
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::RuntimeFailed);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("scripted poll cancelled"));
        QCOMPARE(error_spy.size(), 1);
        QCOMPARE(error_spy.at(0).at(0).toString(), QString("Logging session failed: scripted poll cancelled"));
        QVERIFY(!engine.isRunning());
    }

    void diagnostic_slot_forwards_error_level_with_timestamp_and_linefeed()
    {
        LoggingEngine engine;
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Error)),
                                          Q_ARG(QString, QString("error diagnostic"))));

        QCOMPARE(error_spy.size(), 1);
        QCOMPARE(error_spy.at(0).at(0).toString(), QString("error diagnostic"));
        QCOMPARE(error_spy.at(0).at(1).toBool(), true);
        QCOMPARE(error_spy.at(0).at(2).toBool(), true);
    }

    void diagnostic_slot_forwards_warning_level_with_timestamp_and_linefeed()
    {
        LoggingEngine engine;
        QSignalSpy warning_spy(&engine, &LoggingEngine::LOG_W);

        QVERIFY(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Warning)),
                                          Q_ARG(QString, QString("warning diagnostic"))));

        QCOMPARE(warning_spy.size(), 1);
        QCOMPARE(warning_spy.at(0).at(0).toString(), QString("warning diagnostic"));
        QCOMPARE(warning_spy.at(0).at(1).toBool(), true);
        QCOMPARE(warning_spy.at(0).at(2).toBool(), true);
    }

    void diagnostic_slot_forwards_info_level_with_timestamp_and_linefeed()
    {
        LoggingEngine engine;
        QSignalSpy info_spy(&engine, &LoggingEngine::LOG_I);

        QVERIFY(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Info)),
                                          Q_ARG(QString, QString("info diagnostic"))));

        QCOMPARE(info_spy.size(), 1);
        QCOMPARE(info_spy.at(0).at(0).toString(), QString("info diagnostic"));
        QCOMPARE(info_spy.at(0).at(1).toBool(), true);
        QCOMPARE(info_spy.at(0).at(2).toBool(), true);
    }

    void diagnostic_slot_forwards_debug_level_with_timestamp_and_linefeed()
    {
        LoggingEngine engine;
        QSignalSpy debug_spy(&engine, &LoggingEngine::LOG_D);

        QVERIFY(QMetaObject::invokeMethod(&engine, "handleDiagnostic", Qt::DirectConnection,
                                          Q_ARG(int, static_cast<int>(fastecu::LogLevel::Debug)),
                                          Q_ARG(QString, QString("debug diagnostic"))));

        QCOMPARE(debug_spy.size(), 1);
        QCOMPARE(debug_spy.at(0).at(0).toString(), QString("debug diagnostic"));
        QCOMPARE(debug_spy.at(0).at(1).toBool(), true);
        QCOMPARE(debug_spy.at(0).at(2).toBool(), true);
    }

    void portable_events_map_to_existing_status_and_value_signals()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(PollData{.responded = false});
        protocol->queuePollResult(PollData{.responded = false});
        protocol->queuePollResult(
            PollData{.responded = true, .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "42"}}});
        QSignalSpy status_spy(&engine, &LoggingEngine::statusChanged);
        QSignalSpy value_spy(&engine, &LoggingEngine::valuesUpdated);

        QVERIFY(engine.start(run(std::unique_ptr<LoggingProtocol>(protocol))));
        QVERIFY(value_spy.wait(2000));
        engine.stop();

        QVERIFY(status_spy.size() >= 3);
        QCOMPARE(status_spy.at(0).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        QCOMPARE(status_spy.at(1).at(0).value<LoggingStatus>(), LoggingStatus::CarNotResponding);
        QCOMPARE(status_spy.at(2).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        const auto values = value_spy.at(0).at(0).value<QVector<fastecu::logging::LogSample>>();
        QCOMPARE(values.at(0).channel_id, std::string("rpm"));
        QCOMPARE(values.at(0).numeric_value, 42.0);
    }
};

} // namespace fastecu::desktop::logging

QTEST_GUILESS_MAIN(fastecu::desktop::logging::TestLoggingEngine)
#include "logging_engine_test.moc"
