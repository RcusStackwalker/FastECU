#include <QtTest>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <chrono>

#include "src/backend/logging/testing/scripted_logging_protocol.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/platform/desktop/common/logging/logging_worker.h"

namespace fastecu::desktop::logging
{

namespace
{

using namespace fastecu::logging;

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

LoggingSession session(LoggingPolicy policy = {.poll_timeout_ms = 5,
                                               .car_silence_miss_threshold = 2,
                                               .reconnect_initial_delay_ms = 1000000,
                                               .reconnect_period_ms = 1000,
                                               .max_reconnect_attempts = std::nullopt})
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

class TestLoggingWorker : public QObject
{
    Q_OBJECT
  private slots:
    void forwards_portable_states_samples_and_cancelled_result()
    {
        ScriptedLoggingProtocol protocol;
        protocol.queueStartResult({});
        protocol.queuePollResult(PollData{.responded = false});
        protocol.queuePollResult(PollData{.responded = false});
        protocol.queuePollResult(
            PollData{.responded = true, .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "1234"}}});
        fastecu::FakeClock clock;
        NullDiagnostics diagnostics;
        LoggingWorker worker(session(), &protocol, clock, diagnostics);
        QSignalSpy state_spy(&worker, &LoggingWorker::stateChanged);
        QSignalSpy samples_spy(&worker, &LoggingWorker::samplesReady);
        QSignalSpy finished_spy(&worker, &LoggingWorker::sessionFinished);

        worker.start();
        // Cue off the protocol fake's own condition variable, then join. A
        // QSignalSpy connects with Qt::DirectConnection, so the worker thread
        // records samplesReady itself; QSignalSpy::wait() is edge-triggered
        // and reports only emissions arriving after it snapshots its baseline
        // count, so an emission that lands first makes it burn its whole
        // timeout and return false. Joining is what makes the spies final.
        QVERIFY(protocol.waitUntilQueuedPollResultsConsumed(std::chrono::milliseconds(2000)));
        worker.requestStop();
        QVERIFY(worker.wait(2000));

        QVERIFY(state_spy.size() >= 3);
        QCOMPARE(state_spy.at(0).at(0).value<LoggingState>(), LoggingState::Running);
        QCOMPARE(state_spy.at(1).at(0).value<LoggingState>(), LoggingState::CarNotResponding);
        QCOMPARE(state_spy.at(2).at(0).value<LoggingState>(), LoggingState::Running);
        const auto samples = samples_spy.at(0).at(0).value<QVector<fastecu::logging::LogSample>>();
        QCOMPARE(samples.size(), 1);
        QCOMPARE(samples.at(0).channel_id, std::string("rpm"));
        QCOMPARE(samples.at(0).numeric_value, 1234.0);
        QCOMPARE(finished_spy.size(), 1);
        const auto result = finished_spy.at(0).at(0).value<fastecu::Status>();
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::Cancelled);
        QVERIFY(protocol.stopCalled());
    }

    void forwards_final_start_error_without_policy_mapping()
    {
        ScriptedLoggingProtocol protocol;
        protocol.queueStartResult(fastecu::fail(fastecu::ErrorKind::BadResponse, "handshake rejected"));
        fastecu::FakeClock clock;
        NullDiagnostics diagnostics;
        LoggingWorker worker(session(), &protocol, clock, diagnostics);
        QSignalSpy finished_spy(&worker, &LoggingWorker::sessionFinished);

        worker.start();
        // Joining is sufficient: run() emits sessionFinished last, so once the
        // thread is joined the spy is final. See the note above for why
        // QSignalSpy::wait() is the wrong tool here.
        QVERIFY(worker.wait(2000));

        const auto result = finished_spy.at(0).at(0).value<fastecu::Status>();
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::BadResponse);
        QCOMPARE(result.error().detail, std::string("handshake rejected"));
    }

    void destruction_cancels_and_joins_a_blocked_poll()
    {
        ScriptedLoggingProtocol protocol;
        protocol.queueStartResult({});
        protocol.blockPollUntilCancelled();
        fastecu::FakeClock clock;
        NullDiagnostics diagnostics;
        QElapsedTimer elapsed;
        elapsed.start();
        {
            LoggingWorker worker(session(), &protocol, clock, diagnostics);
            worker.start();
            QVERIFY(protocol.waitUntilPollEntered(std::chrono::milliseconds(500)));
        }

        QVERIFY2(elapsed.elapsed() < 500, "worker destruction exceeded cancellation bound");
        QVERIFY(protocol.stopCalled());
    }
};

} // namespace fastecu::desktop::logging

QTEST_GUILESS_MAIN(fastecu::desktop::logging::TestLoggingWorker)
#include "logging_worker_test.moc"
