#include <QtTest>
#include <QSignalSpy>
#include "logging/logging_engine.h"
#include "scripted_logging_protocol.h"
#include "test_logging_engine.h"

class TestLoggingEngine : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<QVector<LogSample>>();
        qRegisterMetaType<LoggingStatus>();
        qRegisterMetaType<SessionEndReason>();
    }

    void start_with_unregistered_protocol_fails_immediately() {
        LoggingEngine engine;
        LogSessionConfig config;
        config.protocolId = "NOPE";
        QVERIFY(!engine.start(config));
        QVERIFY(!engine.isRunning());
    }

    void start_and_stop_round_trip() {
        LoggingEngine engine;
        auto *proto = new ScriptedLoggingProtocol();
        proto->queueStartResult(true);
        engine.registerProtocol("TEST",
            [proto](const LogSessionConfig &) { return std::unique_ptr<LoggingProtocol>(proto); },
            20, 5, 10, 10);

        LogSessionConfig config;
        config.protocolId = "TEST";
        QVERIFY(engine.start(config));
        QVERIFY(engine.isRunning());

        engine.stop();
        QVERIFY(!engine.isRunning());
    }

    void spontaneous_handshake_failure_propagates_session_ended() {
        LoggingEngine engine;
        auto *proto = new ScriptedLoggingProtocol();
        proto->queueStartResult(false, "no ECU");
        engine.registerProtocol("TEST",
            [proto](const LogSessionConfig &) { return std::unique_ptr<LoggingProtocol>(proto); },
            20, 5, 10, 10);

        QSignalSpy endedSpy(&engine, &LoggingEngine::sessionEnded);

        LogSessionConfig config;
        config.protocolId = "TEST";
        QVERIFY(engine.start(config));
        QVERIFY(endedSpy.wait(2000));

        QCOMPARE(endedSpy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::HandshakeFailed);
    }
};

int run_test_logging_engine(int argc, char** argv) {
    TestLoggingEngine t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_logging_engine.moc"
