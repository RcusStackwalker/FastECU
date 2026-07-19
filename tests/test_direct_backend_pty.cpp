#include "test_direct_backend_pty.h"

#include <QtTest>
#include <thread>

#if defined(__linux__)
#include <pty.h> // openpty
#else
#include <util.h> // openpty
#endif
#include <unistd.h>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Backend behavior over a PTY (spec: reassembly, timeout, buffer clearing,
// adapter-vanish). J2534 ioctl parameter handling stays with the crash suite's
// MockOpenPort + the bench checklist.
class TestDirectBackendPty : public QObject
{
    Q_OBJECT
  private slots:
    void ptyRead_reassemblesFragmentedFrame();
    void ptyRead_timesOutCleanOnSilence();
    void ptyClearRxBuffer_discardsPendingBytes();
    void ptyAdapterVanish_readReturnsCleanly();

  private:
    int openPtyBackend(SerialPortActionsDirect& backend);
};

int TestDirectBackendPty::openPtyBackend(SerialPortActionsDirect& backend)
{
    int master = -1, slave = -1;
    char name[256] = {0};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0)
    {
        return -1;
    }
    backend.serial_port_prefix_linux = "";
    backend.serial_port_list = QStringList() << QString::fromLocal8Bit(name);
    if (backend.open_serial_port() != QString::fromLocal8Bit(name))
    {
        return -1;
    }
    return master;
}

void TestDirectBackendPty::ptyRead_reassemblesFragmentedFrame()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    // The "ECU" delivers one framed message in two fragments with a gap:
    // header first, payload+checksum 30ms later. The reader must reassemble.
    std::thread responder([master]
                          {
        ::write(master, "\x80\xf0\x10\x02", 4);
        QThread::msleep(30);
        ::write(master, "\xaa\xbb\xcc", 3); });
    const QByteArray got = direct.read_serial_data(500);
    responder.join();
    QCOMPARE(got, QByteArray("\x80\xf0\x10\x02\xaa\xbb\xcc", 7));
    ::close(master);
}

void TestDirectBackendPty::ptyRead_timesOutCleanOnSilence()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    QElapsedTimer t;
    t.start();
    QCOMPARE(direct.read_serial_data(150), QByteArray());
    const qint64 elapsed = t.elapsed();
    QVERIFY2(elapsed >= 140 && elapsed < 1000,
             qPrintable(QString("timeout took %1 ms").arg(elapsed)));
    ::close(master);
}

void TestDirectBackendPty::ptyClearRxBuffer_discardsPendingBytes()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    ::write(master, "\x11\x22\x33", 3); // junk arrives...
    QThread::msleep(50);                // ...and lands in the buffer
    direct.clear_rx_buffer();           // must discard it
    QCOMPARE(direct.read_serial_data(100), QByteArray());
    ::close(master);
}

void TestDirectBackendPty::ptyAdapterVanish_readReturnsCleanly()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    ::close(master); // the adapter disappears
    // The read must come back empty (possibly via handle_error ->
    // reset_connection) without crashing or hanging.
    QCOMPARE(direct.read_serial_data(100), QByteArray());
}

int run_test_direct_backend_pty(int argc, char **argv)
{
    TestDirectBackendPty t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_direct_backend_pty.moc"
