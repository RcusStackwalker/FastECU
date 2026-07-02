#include "test_facade_threading.h"

#include <QtTest>
#include <QSemaphore>
#include <thread>

#include "fake_backend.h"
#include "serial_port_actions.h"

class TestFacadeThreading : public QObject
{
    Q_OBJECT
private slots:
    void constructDestroy_withoutUse_noThreadNoHang();
    void getSet_marshalsToBackendThread();
    void scriptedRead_returnsThroughFacade();
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

int run_test_facade_threading(int argc, char **argv)
{
    TestFacadeThreading t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_facade_threading.moc"
