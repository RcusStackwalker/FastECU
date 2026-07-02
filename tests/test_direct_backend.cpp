#include "test_direct_backend.h"

#include <QtTest>
#include <thread>

#include <util.h>      // openpty (macOS)
#include <unistd.h>

#include "serial_backend.h"
#include "serial_port_actions_direct.h"

// Exercises the SerialBackend contract against the direct implementation
// purely through the base-class pointer: get/set roundtrips hit the same
// storage the backend's own I/O logic reads, and closed-port I/O calls
// return their documented empty/error values without hardware.
class TestDirectBackend : public QObject
{
    Q_OBJECT
private slots:
    void getSet_roundtrip_throughInterface();
    void closedPort_ioCalls_returnEmpty();
    // Backend behavior over a PTY (spec: reassembly, timeout, buffer
    // clearing, adapter-vanish). J2534 ioctl parameter handling stays with
    // the crash suite's MockOpenPort + the bench checklist.
    void ptyRead_reassemblesFragmentedFrame();
    void ptyRead_timesOutCleanOnSilence();
    void ptyClearRxBuffer_discardsPendingBytes();
    void ptyAdapterVanish_readReturnsCleanly();

private:
    // Opens a pty pair and the direct backend's plain-serial path on the
    // slave end. Returns the master fd; fills `backend`.
    int openPtyBackend(SerialPortActionsDirect &backend);
};

void TestDirectBackend::getSet_roundtrip_throughInterface()
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    b->set_add_ssm_header(true);
    QCOMPARE(b->get_add_ssm_header(), true);
    QCOMPARE(direct.add_ssm_header, true);   // same storage the I/O paths read

    b->set_serial_port_baudrate("10400");
    QCOMPARE(b->get_serial_port_baudrate(), QString("10400"));
    QCOMPARE(direct.serial_port_baudrate, QString("10400"));

    b->set_kline_startbyte(0x80);
    QCOMPARE(b->get_kline_startbyte(), (uint8_t)0x80);

    b->set_can_source_address(0x7E0);
    QCOMPARE(b->get_can_source_address(), (uint32_t)0x7E0);

    b->set_serial_port_list(QStringList() << "ttyUSB0");
    QCOMPARE(b->get_serial_port_list(), QStringList() << "ttyUSB0");

    QCOMPARE(b->qobject(), static_cast<QObject *>(&direct));
}

void TestDirectBackend::closedPort_ioCalls_returnEmpty()
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    QCOMPARE(b->is_serial_port_open(), false);
    QCOMPARE(b->read_serial_data(50), QByteArray());
    // write_serial_data's `return STATUS_SUCCESS;` converts int 0 through the
    // QByteArray(const char*) ctor => empty array. Pin today's behavior.
    QCOMPARE(b->write_serial_data(QByteArray("\x01\x02", 2)), QByteArray());
    b->waitForSource();                // default no-op must not block or crash
}

int TestDirectBackend::openPtyBackend(SerialPortActionsDirect &backend)
{
    int master = -1, slave = -1;
    char name[256] = {0};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0)
        return -1;
    backend.serial_port_prefix_linux = "";
    backend.serial_port_list = QStringList() << QString::fromLocal8Bit(name);
    if (backend.open_serial_port() != QString::fromLocal8Bit(name))
        return -1;
    return master;
}

void TestDirectBackend::ptyRead_reassemblesFragmentedFrame()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    // The "ECU" delivers one framed message in two fragments with a gap:
    // header first, payload+checksum 30ms later. The reader must reassemble.
    std::thread responder([master] {
        ::write(master, "\x80\xf0\x10\x02", 4);
        QThread::msleep(30);
        ::write(master, "\xaa\xbb\xcc", 3);
    });
    const QByteArray got = direct.read_serial_data(500);
    responder.join();
    QCOMPARE(got, QByteArray("\x80\xf0\x10\x02\xaa\xbb\xcc", 7));
    ::close(master);
}

void TestDirectBackend::ptyRead_timesOutCleanOnSilence()
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

void TestDirectBackend::ptyClearRxBuffer_discardsPendingBytes()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    ::write(master, "\x11\x22\x33", 3);          // junk arrives...
    QThread::msleep(50);                          // ...and lands in the buffer
    direct.clear_rx_buffer();                     // must discard it
    QCOMPARE(direct.read_serial_data(100), QByteArray());
    ::close(master);
}

void TestDirectBackend::ptyAdapterVanish_readReturnsCleanly()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    ::close(master);                              // the adapter disappears
    // The read must come back empty (possibly via handle_error ->
    // reset_connection) without crashing or hanging.
    QCOMPARE(direct.read_serial_data(100), QByteArray());
}

int run_test_direct_backend(int argc, char **argv)
{
    TestDirectBackend t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_direct_backend.moc"
