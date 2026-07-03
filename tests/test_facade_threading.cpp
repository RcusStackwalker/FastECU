#include "test_facade_threading.h"

#include <QtTest>
#include <QSemaphore>
#include <QTimer>
#include <QElapsedTimer>
#include <thread>
#include <vector>

#include "fake_backend.h"
#include "serial_port_actions.h"

class TestFacadeThreading : public QObject
{
    Q_OBJECT
private slots:
    void constructDestroy_withoutUse_noThreadNoHang();
    void getSet_marshalsToBackendThread();
    void scriptedRead_returnsThroughFacade();
    void workerThreadCaller_noAffinityWarnings();
    void concurrentCallers_serializeWithoutInterleaving();
    void guiThreadCaller_compatShim_keepsEventLoopAlive();
    void destroyAfterUse_joinsIoThread();
};

void TestFacadeThreading::constructDestroy_withoutUse_noThreadNoHang()
{
    QElapsedTimer t;
    t.start();
    {
        SerialPortActions serial;   // never used: the I/O thread must not start
    }
    QVERIFY2(t.elapsed() < 1000, "unused facade must construct/destruct instantly");
}

void TestFacadeThreading::getSet_marshalsToBackendThread()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });

    QVERIFY(serial.set_add_ssm_header(true));      // first call: starts the I/O thread
    QVERIFY(fake != nullptr);
    QVERIFY2(fake->thread() != QThread::currentThread(),
             "backend must live on the I/O thread, not the caller's");
    QCOMPARE(serial.get_add_ssm_header(), true);

    serial.set_serial_port_baudrate("10400");
    QCOMPARE(serial.get_serial_port_baudrate(), QString("10400"));
}

void TestFacadeThreading::scriptedRead_returnsThroughFacade()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });

    serial.set_add_ssm_header(false);   // force backend creation
    fake->scriptedResponse = QByteArray("\x80\xf0\x10\x02\xaa\xbb\x11", 7);

    QCOMPARE(serial.read_serial_data(100), fake->scriptedResponse);
    const QStringList log = fake->takeCallLog();
    QCOMPARE(log.first(), QString("read:begin:t=100"));
    QCOMPARE(log.last(), QString("read:end"));
}

// ---- affinity-warning capture ------------------------------------------
static QStringList g_threadWarnings;
static QtMessageHandler g_prevHandler = nullptr;

static void warningCapture(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg &&
        (msg.contains("another thread") || msg.contains("different thread")))
        g_threadWarnings.append(msg);
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

void TestFacadeThreading::workerThreadCaller_noAffinityWarnings()
{
    // The exact LoggingWorker scenario from the bench checklist: a non-GUI
    // thread drives the facade. Data must arrive and Qt must emit no
    // cross-thread affinity warnings.
    g_threadWarnings.clear();
    g_prevHandler = qInstallMessageHandler(warningCapture);

    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });

    QByteArray got;
    std::thread worker([&] {
        serial.set_add_ssm_header(true);
        fake->scriptedResponse = QByteArray("\x80\xf0\x10\x01\x55\x66", 6);
        got = serial.read_serial_data(100);
    });
    worker.join();

    qInstallMessageHandler(g_prevHandler);
    QCOMPARE(got, QByteArray("\x80\xf0\x10\x01\x55\x66", 6));
    QVERIFY2(g_threadWarnings.isEmpty(),
             qPrintable("affinity warnings: " + g_threadWarnings.join(" | ")));
}

void TestFacadeThreading::concurrentCallers_serializeWithoutInterleaving()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);   // create backend
    fake->readDelayMs = 20;             // widen the interleave window
    fake->takeCallLog();

    auto hammer = [&serial](int n) {
        for (int i = 0; i < n; ++i)
        {
            serial.write_serial_data(QByteArray(1, char(i)));
            serial.read_serial_data(10);
        }
    };
    std::thread a(hammer, 5), b(hammer, 5);
    a.join();
    b.join();

    // Every begin must be immediately followed by its matching end: an
    // interleaved wire access would show two consecutive "begin" entries.
    const QStringList log = fake->takeCallLog();
    QCOMPARE(log.size(), 40);   // 2 threads * 5 iterations * 2 ops * (begin+end)
    for (int i = 0; i < log.size(); i += 2)
    {
        QVERIFY2(log.at(i).contains(":begin"), qPrintable(log.at(i)));
        QVERIFY2(log.at(i + 1).contains(":end"), qPrintable(log.at(i + 1)));
        QCOMPARE(log.at(i).section(':', 0, 0), log.at(i + 1).section(':', 0, 0));
    }
}

void TestFacadeThreading::guiThreadCaller_compatShim_keepsEventLoopAlive()
{
    // Main thread == qApp->thread(): the compat shim path. A timer must keep
    // firing while the facade blocks in a 300ms backend call — this is what
    // keeps flash-module progress UI painting until spec 1b.
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    fake->readDelayMs = 300;

    int ticks = 0;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&ticks] { ticks++; });
    timer.start();

    serial.read_serial_data(10);   // blocks ~300ms on the fake backend
    timer.stop();

    QVERIFY2(ticks >= 5, qPrintable(QString("timer only ticked %1 times").arg(ticks)));
}

void TestFacadeThreading::destroyAfterUse_joinsIoThread()
{
    QPointer<QThread> ioThread;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(true);
        ioThread = fake->thread();
        QVERIFY(ioThread && ioThread->isRunning());
    }
    QVERIFY2(!ioThread || !ioThread->isRunning(),
             "facade destruction must stop and join the I/O thread");
}

int run_test_facade_threading(int argc, char **argv)
{
    TestFacadeThreading t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_facade_threading.moc"
