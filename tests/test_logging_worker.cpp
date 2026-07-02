#include <QtTest>
#include <QSignalSpy>
#include "logging/logging_worker.h"
#include "scripted_logging_protocol.h"
#include "test_logging_worker.h"

class TestLoggingWorker : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<QVector<LogSample>>();
        qRegisterMetaType<LoggingStatus>();
        qRegisterMetaType<SessionEndReason>();
    }

    void normal_run_emits_running_then_values() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        PollResult ok1;
        ok1.status = PollResult::Status::Ok;
        ok1.samples.append(LogSample{0, "1234"});
        proto.queuePollResult(ok1);

        LoggingWorker worker(&proto, 20, 5, 10, 10);
        QSignalSpy statusSpy(&worker, &LoggingWorker::statusChanged);
        QSignalSpy valuesSpy(&worker, &LoggingWorker::valuesUpdated);

        worker.start();
        QVERIFY(valuesSpy.wait(2000));
        worker.requestStop();
        QVERIFY(worker.wait(2000));

        QVERIFY(statusSpy.size() >= 1);
        QCOMPARE(statusSpy.at(0).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        QVector<LogSample> samples = valuesSpy.at(0).at(0).value<QVector<LogSample>>();
        QCOMPARE(samples.size(), 1);
        QCOMPARE(samples.at(0).logValueIndex, 0);
        QCOMPARE(samples.at(0).displayValue, QString("1234"));
    }

    void handshake_failure_ends_session_without_polling() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(false, "handshake rejected");

        LoggingWorker worker(&proto, 20, 5, 10, 10);
        QSignalSpy endedSpy(&worker, &LoggingWorker::sessionEnded);

        worker.start();
        QVERIFY(worker.wait(2000));

        QCOMPARE(endedSpy.size(), 1);
        QCOMPARE(endedSpy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::HandshakeFailed);
        QCOMPARE(endedSpy.at(0).at(1).toString(), QString("handshake rejected"));
    }

    void car_dropout_then_recovery_flips_status_back() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        for (int i = 0; i < 5; ++i) {
            PollResult r;
            r.status = PollResult::Status::NoResponse;
            proto.queuePollResult(r);
        }
        PollResult recovered;
        recovered.status = PollResult::Status::Ok;
        recovered.samples.append(LogSample{0, "1"});
        proto.queuePollResult(recovered);

        LoggingWorker worker(&proto, 5, 5, 1000, 1000);
        QSignalSpy statusSpy(&worker, &LoggingWorker::statusChanged);

        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(statusSpy.size(), 3, 3000);
        worker.requestStop();
        QVERIFY(worker.wait(2000));

        QCOMPARE(statusSpy.at(0).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        QCOMPARE(statusSpy.at(1).at(0).value<LoggingStatus>(), LoggingStatus::CarNotResponding);
        QCOMPARE(statusSpy.at(2).at(0).value<LoggingStatus>(), LoggingStatus::Running);
    }

    void adapter_loss_ends_session_and_thread_exits() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        PollResult err;
        err.status = PollResult::Status::TransportError;
        err.errorMessage = "port closed";
        proto.queuePollResult(err);

        LoggingWorker worker(&proto, 20, 5, 10, 10);
        QSignalSpy endedSpy(&worker, &LoggingWorker::sessionEnded);

        worker.start();
        QVERIFY(worker.wait(2000));

        QCOMPARE(endedSpy.size(), 1);
        QCOMPARE(endedSpy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::AdapterDisconnected);
        QVERIFY(proto.stopCalled());
    }

    void stop_is_noticed_within_one_poll_cycle() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        for (int i = 0; i < 1000; ++i) {
            PollResult r;
            r.status = PollResult::Status::NoResponse;
            proto.queuePollResult(r);
        }

        LoggingWorker worker(&proto, 10, 100000, 100000, 100000);
        worker.start();
        QTest::qWait(30);
        worker.requestStop();
        QVERIFY(worker.wait(500));
    }
};

int run_test_logging_worker(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    TestLoggingWorker t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_logging_worker.moc"
