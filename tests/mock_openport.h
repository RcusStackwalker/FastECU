#ifndef MOCK_OPENPORT_H
#define MOCK_OPENPORT_H

#include <QObject>
#include <QSemaphore>
#include <QSocketNotifier>
#include <QThread>
#include <atomic>
#include <unistd.h>

// Mock Tactrix Openport 2.0 on the master side of a pseudo-terminal. FastECU's
// real QSerialPort opens the slave; this responder speaks just enough of the
// Openport serial protocol for PassThruOpen + PassThruReadVersion to succeed:
//   ata  (open)         -> "ari\r\n"            (any "ar.." reply passes the check)
//   ati  (read version) -> "ari 1.17.4877\r\n"  (firmware parsed from after "ari ")
//   other at* (connect/filters/ioctls) -> "aro\r\n" (generic ack)
// Driven by a QSocketNotifier on a dedicated thread, so it responds like a real
// adapter regardless of whether the calling thread pumps the event loop.
// Designed to run on MockOpenPortThread.
class MockOpenPort : public QObject
{
    Q_OBJECT
  public:
    explicit MockOpenPort(int masterFd, QObject *parent = nullptr)
        : QObject(parent), fd(masterFd),
          notifier(new QSocketNotifier(masterFd, QSocketNotifier::Read, this))
    {
        connect(notifier, &QSocketNotifier::activated, this, &MockOpenPort::onReadable);
    }

    // When false, the READ_VBATT command ("atr ...") gets no reply, so the
    // caller's read stays parked in read_serial_data's event-loop pump — used to
    // make the "reset while a read is in-flight" window deterministic.
    std::atomic<bool> answerReadVbatt{true};

  private slots:
    void onReadable()
    {
        char buf[256];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0)
            return;
        rx.append(buf, static_cast<int>(n));

        int nl;
        while ((nl = rx.indexOf('\n')) >= 0)
        {
            const QByteArray line = rx.left(nl).trimmed();
            rx.remove(0, nl + 1);
            if (line.isEmpty())
                continue;

            if (line.contains("atr") && !answerReadVbatt)
                continue; // withhold READ_VBATT reply: keep the caller's read parked

            QByteArray resp;
            if (line.contains("ati"))
                resp = "ari 1.17.4877\r\n";
            else if (line.contains("ata"))
                resp = "ari\r\n";
            else
                resp = "aro\r\n";
            ::write(fd, resp.constData(), resp.size());
        }
    }

  private:
    int fd;
    QSocketNotifier *notifier;
    QByteArray rx;
};

// Runs a MockOpenPort on its own thread with its own event loop, so it
// responds like a real adapter regardless of whether the code under test
// pumps events. Responding starts before the constructor returns.
class MockOpenPortThread : public QThread
{
  public:
    explicit MockOpenPortThread(int masterFd) : fd(masterFd)
    {
        start();
        ready.acquire();
    }
    ~MockOpenPortThread() override
    {
        quit();
        wait();
    }

    // Safe to flip from the test thread at any time (std::atomic member).
    MockOpenPort *mock = nullptr;

  protected:
    void run() override
    {
        MockOpenPort m(fd); // created here => notifier lives on this thread
        mock = &m;
        ready.release();
        exec();
        mock = nullptr;
    }

  private:
    int fd;
    QSemaphore ready;
};

#endif // MOCK_OPENPORT_H
