#include <QtTest>
#include <QElapsedTimer>
#include <QSignalSpy>

#include "scripted_logging_protocol.h"
#include "src/platform/desktop/common/logging/logging_worker.h"
#include "test_logging_worker.h"

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
                                               .reconnect_attempt_threshold = 1000,
                                               .reconnect_retry_period = 0})
{
    auto result = make_logging_session(
        LoggingProtocolId::Ssm,
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
        protocol.queuePollResult(PollData{
            .responded = true,
            .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "1234"}}});
        NullDiagnostics diagnostics;
        LoggingWorker worker(session(), &protocol, diagnostics);
        QSignalSpy state_spy(&worker, &LoggingWorker::stateChanged);
        QSignalSpy samples_spy(&worker, &LoggingWorker::samplesReady);
        QSignalSpy finished_spy(&worker, &LoggingWorker::sessionFinished);

        worker.start();
        QVERIFY(samples_spy.wait(2000));
        worker.requestStop();
        QVERIFY(worker.wait(500));

        QVERIFY(state_spy.size() >= 3);
        QCOMPARE(state_spy.at(0).at(0).value<LoggingState>(), LoggingState::Running);
        QCOMPARE(state_spy.at(1).at(0).value<LoggingState>(), LoggingState::CarNotResponding);
        QCOMPARE(state_spy.at(2).at(0).value<LoggingState>(), LoggingState::Running);
        const auto samples =
            samples_spy.at(0).at(0).value<QVector<fastecu::logging::LogSample>>();
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
        protocol.queueStartResult(
            fastecu::fail(fastecu::ErrorKind::BadResponse, "handshake rejected"));
        NullDiagnostics diagnostics;
        LoggingWorker worker(session(), &protocol, diagnostics);
        QSignalSpy finished_spy(&worker, &LoggingWorker::sessionFinished);

        worker.start();
        QVERIFY(finished_spy.wait(2000));
        QVERIFY(worker.wait(500));

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
        NullDiagnostics diagnostics;
        QElapsedTimer elapsed;
        elapsed.start();
        {
            LoggingWorker worker(session(), &protocol, diagnostics);
            worker.start();
            QVERIFY(protocol.waitUntilPollEntered(std::chrono::milliseconds(500)));
        }

        QVERIFY2(elapsed.elapsed() < 500, "worker destruction exceeded cancellation bound");
        QVERIFY(protocol.stopCalled());
    }
};

int run_test_logging_worker(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestLoggingWorker test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_logging_worker.moc"
