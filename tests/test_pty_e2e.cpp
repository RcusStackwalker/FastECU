#include "test_pty_e2e.h"

#include <QtTest>
#include <atomic>
#include <thread>

#if defined(__linux__)
#include <pty.h> // openpty
#else
#include <util.h> // openpty
#endif
#include <unistd.h>
#include <poll.h>

#include "serial_port_actions.h"

// End-to-end over a pseudo-terminal, all through the production stack:
// facade -> I/O thread -> real SerialPortActionsDirect -> QSerialPort(pty).
// The caller is a WORKER thread (the LoggingWorker scenario, bench checklist
// item 1); the "ECU" is a responder thread on the pty master.
class TestPtyE2e : public QObject
{
    Q_OBJECT
  private slots:
    void workerThread_writeRead_overPty_deliversFramedMessage();
};

void TestPtyE2e::workerThread_writeRead_overPty_deliversFramedMessage()
{
    int master = -1, slave = -1;
    char name[256] = {0};
    QVERIFY2(openpty(&master, &slave, name, nullptr, nullptr) == 0, "openpty failed");

    // ECU responder: on receiving anything, replies with one SSM-framed
    // message (header 80 f0 10, len 02, payload aa bb, checksum byte).
    //
    // DEVIATION from the plan's verbatim listing: the responder polls with a
    // short timeout instead of blocking indefinitely in ::read(), and is
    // joined *before* ::close(master) below (rather than relying on close()
    // to unblock a concurrent blocking read()). Root-caused via `sample` on
    // a hung run plus an isolated non-Qt openpty()/close()/read() repro:
    // on this macOS/Darwin kernel, calling close() on a pty master fd from
    // one thread while another thread has a pending blocking read() on that
    // same fd does not just leave the reader blocked (as it reliably does on
    // Linux) -- close() itself never returns, and the process becomes
    // unkillable even with SIGKILL (stuck in an uninterruptible kernel
    // wait). Confirmed with a bare-bones repro (no Qt/production code) that
    // reproduces the identical hang, and that switching to poll()+join
    // avoids it entirely. This is not a Task 5 marshaling defect: the
    // production worker thread (open/write/read through the facade) had
    // already completed and been joined by the time the hang occurred.
    std::atomic<bool> stop{false};
    QByteArray received;
    std::thread responder([&]
                          {
        const char reply[] = "\x80\xf0\x10\x02\xaa\xbb\xcc";
        char buf[64];
        bool replied = false;
        while (!stop.load())
        {
            struct pollfd pfd { master, POLLIN, 0 };
            int pr = ::poll(&pfd, 1, 100);   // 100ms: re-check stop periodically
            if (pr <= 0)
                continue;
            ssize_t n = ::read(master, buf, sizeof(buf));
            if (n > 0)
            {
                received.append(buf, int(n));
                if (!replied) { ::write(master, reply, 7); replied = true; }
            }
            else if (n < 0)
                break;
        } });

    SerialPortActions serial; // default factory: real direct backend
    QByteArray response;
    QString opened;
    std::thread worker([&]
                       {
        serial.set_serial_port_prefix_linux("");
        serial.set_serial_port_list(QStringList() << QString::fromLocal8Bit(name));
        opened = serial.open_serial_port();
        serial.write_serial_data(QByteArray("\x01\x02\x03", 3));
        response = serial.read_serial_data(2000); });
    worker.join();
    stop.store(true);
    responder.join(); // joins on its own via the poll timeout + stop check
    ::close(master);

    QCOMPARE(opened, QString::fromLocal8Bit(name));
    QCOMPARE(received.left(3), QByteArray("\x01\x02\x03", 3));
    QCOMPARE(response, QByteArray("\x80\xf0\x10\x02\xaa\xbb\xcc", 7));
}

int run_test_pty_e2e(int argc, char **argv)
{
    TestPtyE2e t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_pty_e2e.moc"
