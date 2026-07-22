#include "test_facade_threading.h"

#include <QtTest>
#include <QCoreApplication>
#include <QSemaphore>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include "fake_backend.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

namespace
{
class MutableCancellationToken final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return cancelled_.load();
    }

    void cancel()
    {
        cancelled_.store(true);
    }

    void reset()
    {
        cancelled_.store(false);
    }

  private:
    std::atomic<bool> cancelled_{false};
};
} // namespace

class TestFacadeThreading : public QObject
{
    Q_OBJECT
  private slots:
    void constructDestroy_withoutUse_noThreadNoHang();
    void getSet_marshalsToBackendThread();
    void scriptedRead_returnsThroughFacade();
    void backendException_propagatesWithoutHangingAndCleansUp();
    void transportAdapters_isOpenContainsBackendException();
    void transportAdapters_normalEmptyReadIsSuccess();
    void transportAdapters_preCancelledReadSkipsBackend();
    void transportAdapters_postCallCancellationPrecedesDisconnect();
    void transportAdapters_backendReadExceptionMapsToInternal();
    void canTransport_truncatedFrameMapsToInternal();
    void workerThreadCaller_noAffinityWarnings();
    void concurrentCallers_serializeWithoutInterleaving();
    void destroyAfterUse_joinsIoThread();
    void destroyWhileReadInFlight_waitsForBackendCall();
};

void TestFacadeThreading::constructDestroy_withoutUse_noThreadNoHang()
{
    QElapsedTimer t;
    t.start();
    {
        SerialPortActions serial; // never used: the I/O thread must not start
    }
    QVERIFY2(t.elapsed() < 1000, "unused facade must construct/destruct instantly");
}

void TestFacadeThreading::getSet_marshalsToBackendThread()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });

    QVERIFY(serial.set_add_ssm_header(true)); // first call: starts the I/O thread
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
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });

    serial.set_add_ssm_header(false); // force backend creation
    fake->scriptedResponse = QByteArray("\x80\xf0\x10\x02\xaa\xbb\x11", 7);

    QCOMPARE(serial.read_serial_data(100), fake->scriptedResponse);
    const QStringList log = fake->takeCallLog();
    QCOMPARE(log.first(), QString("read:begin:t=100"));
    QCOMPARE(log.last(), QString("read:end"));
}

void TestFacadeThreading::backendException_propagatesWithoutHangingAndCleansUp()
{
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FASTECU_THROWING_BACKEND_CHILD", "1");
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY2(child.waitForStarted(1000), qPrintable(child.errorString()));

    if (!child.waitForFinished(2000))
    {
        child.kill();
        child.waitForFinished(1000);
        QFAIL("backend exception left the facade caller blocked");
    }

    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void TestFacadeThreading::transportAdapters_isOpenContainsBackendException()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fake->throwOnIsOpen = true;

    bool open = true;
    try
    {
        open = ssm.isOpen();
    }
    catch (...)
    {
        QFAIL("SSM standalone isOpen propagated a backend exception");
    }
    QVERIFY(!open);

    open = true;
    try
    {
        open = kline.isOpen();
    }
    catch (...)
    {
        QFAIL("K-Line standalone isOpen propagated a backend exception");
    }
    QVERIFY(!open);

    open = true;
    try
    {
        open = can.isOpen();
    }
    catch (...)
    {
        QFAIL("CAN standalone isOpen propagated a backend exception");
    }
    QVERIFY(!open);
}

void TestFacadeThreading::transportAdapters_normalEmptyReadIsSuccess()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    MutableCancellationToken cancellation;

    const auto ssmResult = ssm.read(10, cancellation);
    QVERIFY(ssmResult.has_value());
    QVERIFY(!ssmResult->has_value());

    const auto klineResult = kline.read(10, cancellation);
    QVERIFY(klineResult.has_value());
    QVERIFY(!klineResult->has_value());

    const auto canResult = can.read(10, cancellation);
    QVERIFY(canResult.has_value());
    QVERIFY(!canResult->has_value());
}

void TestFacadeThreading::transportAdapters_preCancelledReadSkipsBackend()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    fake->takeCallLog();
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    MutableCancellationToken cancellation;
    cancellation.cancel();

    const auto ssmResult = ssm.read(10, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Cancelled);

    const auto klineResult = kline.read(10, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Cancelled);

    const auto canResult = can.read(10, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Cancelled);
    QVERIFY(fake->takeCallLog().isEmpty());
}

void TestFacadeThreading::transportAdapters_postCallCancellationPrecedesDisconnect()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    MutableCancellationToken cancellation;
    fake->afterRead = [&]
    {
        cancellation.cancel();
        fake->portOpen.store(false);
    };

    const auto ssmResult = ssm.read(10, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Cancelled);

    cancellation.reset();
    fake->portOpen.store(true);
    const auto klineResult = kline.read(10, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Cancelled);

    cancellation.reset();
    fake->portOpen.store(true);
    const auto canResult = can.read(10, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Cancelled);
}

void TestFacadeThreading::transportAdapters_backendReadExceptionMapsToInternal()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    MutableCancellationToken cancellation;
    fake->throwOnRead = true;

    const auto ssmResult = ssm.read(10, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Internal);

    const auto klineResult = kline.read(10, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Internal);

    const auto canResult = can.read(10, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Internal);
}

void TestFacadeThreading::canTransport_truncatedFrameMapsToInternal()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    fake->scriptedResponse = QByteArray("\x01\x02\x03", 3);
    cdbg::FastEcuCanTransport can(&serial);
    MutableCancellationToken cancellation;

    const auto result = can.read(10, cancellation);
    QVERIFY(!result.has_value());
    QVERIFY(result.error().kind == fastecu::ErrorKind::Internal);
}

// ---- affinity-warning capture ------------------------------------------
static QStringList g_threadWarnings;
static QtMessageHandler g_prevHandler = nullptr;

static void warningCapture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type == QtWarningMsg &&
        (msg.contains("another thread") || msg.contains("different thread")))
    {
        g_threadWarnings.append(msg);
    }
    if (g_prevHandler)
    {
        g_prevHandler(type, ctx, msg);
    }
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
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });

    QByteArray got;
    std::thread worker([&]
                       {
        serial.set_add_ssm_header(true);
        fake->scriptedResponse = QByteArray("\x80\xf0\x10\x01\x55\x66", 6);
        got = serial.read_serial_data(100); });
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
                             [&fake]() -> SerialBackend *
                             { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false); // create backend
    fake->readDelayMs = 20;           // widen the interleave window
    fake->takeCallLog();

    auto hammer = [&serial](int n)
    {
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
    QCOMPARE(log.size(), 40); // 2 threads * 5 iterations * 2 ops * (begin+end)
    for (int i = 0; i < log.size(); i += 2)
    {
        QVERIFY2(log.at(i).contains(":begin"), qPrintable(log.at(i)));
        QVERIFY2(log.at(i + 1).contains(":end"), qPrintable(log.at(i + 1)));
        QCOMPARE(log.at(i).section(':', 0, 0), log.at(i + 1).section(':', 0, 0));
    }
}

void TestFacadeThreading::destroyAfterUse_joinsIoThread()
{
    QPointer<QThread> ioThread;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(true);
        ioThread = fake->thread();
        QVERIFY(ioThread && ioThread->isRunning());
    }
    QVERIFY2(!ioThread || !ioThread->isRunning(),
             "facade destruction must stop and join the I/O thread");
}

void TestFacadeThreading::destroyWhileReadInFlight_waitsForBackendCall()
{
    FakeBackend *fake = nullptr;
    auto *serial = new SerialPortActions(
        "", "", nullptr, nullptr,
        [&fake]() -> SerialBackend *
        { fake = new FakeBackend(); return fake; });

    serial->set_add_ssm_header(false); // create backend before wiring gates
    fake->scriptedResponse = QByteArray("done");

    QSemaphore readEntered;
    QSemaphore continueRead;
    fake->readEntered = &readEntered;
    fake->continueRead = &continueRead;

    QByteArray got;
    std::thread reader([&]
                       { got = serial->read_serial_data(10); });
    QVERIFY2(readEntered.tryAcquire(1, 1000), "backend read did not start");

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&]
                          {
        delete serial;
        destroyed.store(true); });

    QTest::qWait(50);
    QVERIFY2(!destroyed.load(),
             "facade teardown must wait for the in-flight backend call");

    continueRead.release();
    reader.join();
    destroyer.join();

    QCOMPARE(got, QByteArray("done"));
    QVERIFY(destroyed.load());
}

int run_test_facade_threading(int argc, char **argv)
{
    TestFacadeThreading t;
    return QTest::qExec(&t, argc, argv);
}

int run_throwing_backend_child()
{
    bool backendDestroyed = false;
    bool exceptionPropagated = false;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(false);
        fake->throwOnRead = true;
        fake->destroyed = &backendDestroyed;

        try
        {
            serial.read_serial_data(10);
        }
        catch (const std::runtime_error& error)
        {
            exceptionPropagated = QString::fromUtf8(error.what()) ==
                                  QStringLiteral("scripted backend read failure");
        }
    }
    return exceptionPropagated && backendDestroyed ? 0 : 1;
}

#include "test_facade_threading.moc"
