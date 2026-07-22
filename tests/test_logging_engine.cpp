#include <QtTest>
#include <QSignalSpy>

#include "scripted_logging_protocol.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "test_logging_engine.h"

namespace
{

using fastecu::desktop::logging::DesktopLoggingSnapshot;
using namespace fastecu::logging;

DesktopLoggingSnapshot snapshot()
{
    auto session = make_logging_session(
        LoggingProtocolId::Ssm,
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
    Q_ASSERT(session.has_value());
    return DesktopLoggingSnapshot{.session = std::move(*session),
                                  .index_by_id = {{"rpm", 0}},
                                  .enabled_ids = {"rpm"}};
}

} // namespace

class TestLoggingEngine : public QObject
{
    Q_OBJECT
  private slots:
    void unregistered_protocol_fails_immediately()
    {
        LoggingEngine engine;
        QVERIFY(!engine.start(LogSessionConfig{.protocolId = "NOPE"}, snapshot()));
        QVERIFY(!engine.isRunning());
    }

    void factory_receives_validated_snapshot_and_user_stop_is_silent()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->blockPollUntilCancelled();
        bool saw_session = false;
        engine.registerProtocol(
            "TEST", [protocol, &saw_session](const DesktopLoggingSnapshot& value)
            {
                saw_session = value.session.find_channel("rpm") != nullptr;
                return std::unique_ptr<LoggingProtocol>(protocol); });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));
        QVERIFY(protocol->waitUntilPollEntered(std::chrono::milliseconds(500)));
        QVERIFY(saw_session);
        QVERIFY(engine.isRunning());

        engine.stop();
        QVERIFY(!engine.isRunning());
        QTest::qWait(20);
        QCOMPARE(ended_spy.size(), 0);
        QCOMPARE(error_spy.size(), 0);
    }

    void null_factory_result_fails_without_starting()
    {
        LoggingEngine engine;
        engine.registerProtocol("TEST", [](const DesktopLoggingSnapshot&)
                                { return std::unique_ptr<LoggingProtocol>(); });
        QVERIFY(!engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));
        QVERIFY(!engine.isRunning());
    }

    void factory_error_preserves_structured_kind_and_detail()
    {
        LoggingEngine engine;
        engine.registerProtocol(
            "TEST", [](const DesktopLoggingSnapshot&) -> fastecu::Result<std::unique_ptr<LoggingProtocol>>
            { return fastecu::fail(fastecu::ErrorKind::Disconnected, "open failed"); });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(!engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));

        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(),
                 SessionEndReason::AdapterDisconnected);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("open failed"));
        QVERIFY(!engine.isRunning());
    }

    void factory_exception_is_contained_at_platform_boundary()
    {
        LoggingEngine engine;
        engine.registerProtocol(
            "TEST", [](const DesktopLoggingSnapshot&) -> std::unique_ptr<LoggingProtocol>
            { throw std::runtime_error("driver setup exploded"); });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(!engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));

        QCOMPARE(ended_spy.size(), 1);
        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(),
                 SessionEndReason::HandshakeFailed);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("driver setup exploded"));
    }

    void start_error_preserves_handshake_failure_ui_path()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult(
            fastecu::fail(fastecu::ErrorKind::BadResponse, "no ECU"));
        engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                                { return std::unique_ptr<LoggingProtocol>(protocol); });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(),
                 SessionEndReason::HandshakeFailed);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("no ECU"));
        QCOMPARE(error_spy.at(0).at(0).toString(),
                 QString("Logging session failed to start: no ECU"));
        QVERIFY(!engine.isRunning());
    }

    void disconnect_error_preserves_adapter_failure_ui_path()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(
            fastecu::fail(fastecu::ErrorKind::Disconnected, "port closed"));
        engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                                { return std::unique_ptr<LoggingProtocol>(protocol); });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);

        QVERIFY(engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(),
                 SessionEndReason::AdapterDisconnected);
        QCOMPARE(ended_spy.at(0).at(1).toString(), QString("port closed"));
        QVERIFY(!engine.isRunning());
    }

    void post_start_failure_is_not_reported_as_handshake_failure()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(
            fastecu::fail(fastecu::ErrorKind::Internal, "bad stream frame"));
        engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                                { return std::unique_ptr<LoggingProtocol>(protocol); });
        QSignalSpy ended_spy(&engine, &LoggingEngine::sessionEnded);
        QSignalSpy error_spy(&engine, &LoggingEngine::LOG_E);

        QVERIFY(engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));
        QVERIFY(ended_spy.wait(2000));

        QCOMPARE(ended_spy.at(0).at(0).value<SessionEndReason>(),
                 SessionEndReason::RuntimeFailed);
        QCOMPARE(error_spy.at(0).at(0).toString(),
                 QString("Logging session failed: bad stream frame"));
    }

    void portable_events_map_to_existing_status_and_value_signals()
    {
        LoggingEngine engine;
        auto *protocol = new ScriptedLoggingProtocol();
        protocol->queueStartResult({});
        protocol->queuePollResult(PollData{.responded = false});
        protocol->queuePollResult(PollData{.responded = false});
        protocol->queuePollResult(PollData{
            .responded = true,
            .samples = {ProtocolSample{.channel_id = "rpm", .raw_value = "42"}}});
        engine.registerProtocol("TEST", [protocol](const DesktopLoggingSnapshot&)
                                { return std::unique_ptr<LoggingProtocol>(protocol); });
        QSignalSpy status_spy(&engine, &LoggingEngine::statusChanged);
        QSignalSpy value_spy(&engine, &LoggingEngine::valuesUpdated);

        QVERIFY(engine.start(LogSessionConfig{.protocolId = "TEST"}, snapshot()));
        QVERIFY(value_spy.wait(2000));
        engine.stop();

        QVERIFY(status_spy.size() >= 3);
        QCOMPARE(status_spy.at(0).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        QCOMPARE(status_spy.at(1).at(0).value<LoggingStatus>(),
                 LoggingStatus::CarNotResponding);
        QCOMPARE(status_spy.at(2).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        const auto values =
            value_spy.at(0).at(0).value<QVector<fastecu::logging::LogSample>>();
        QCOMPARE(values.at(0).channel_id, std::string("rpm"));
        QCOMPARE(values.at(0).numeric_value, 42.0);
    }
};

int run_test_logging_engine(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestLoggingEngine test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_logging_engine.moc"
