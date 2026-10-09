#pragma once

#include <QObject>
#include <QSemaphore>
#include <QSocketNotifier>
#include <QThread>
#include <array>
#include <atomic>
#include <tuple>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#else
int read(int fd, char *buf, std::size_t size);
int write(int fd, const char *buf, std::size_t size);
#endif

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
    explicit MockOpenPort(int master_fd, QObject *parent = nullptr)
        : QObject(parent), fd_(master_fd), notifier_(new QSocketNotifier(master_fd, QSocketNotifier::Read, this))
    {
        connect(notifier_, &QSocketNotifier::activated, this, &MockOpenPort::onReadable);
    }

    // When false, the READ_VBATT command ("atr ...") gets no reply, so the
    // caller's read stays parked in read_serial_data's event-loop pump — used to
    // make the "reset while a read is in-flight" window deterministic.
    std::atomic<bool> answer_read_vbatt{true};

  private slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void onReadable()
    {
        std::array<char, 256> buf{};
        const auto n = ::read(fd_, buf.data(), buf.size());
        if (n <= 0)
        {
            return;
        }
        rx_.append(buf.data(), static_cast<int>(n));

        qsizetype nl;
        while ((nl = rx_.indexOf('\n')) >= 0)
        {
            const QByteArray line = rx_.left(nl).trimmed();
            rx_.remove(0, nl + 1);
            if (line.isEmpty())
            {
                continue;
            }

            if (line.contains("atr") && !answer_read_vbatt)
            {
                continue; // withhold READ_VBATT reply: keep the caller's read parked
            }

            QByteArray resp;
            if (line.contains("ati"))
            {
                resp = "ari 1.17.4877\r\n";
            }
            else if (line.contains("ata"))
            {
                resp = "ari\r\n";
            }
            else
            {
                resp = "aro\r\n";
            }
            // A short write surfaces as a missing reply, which the caller's
            // read timeout reports.
            std::ignore = ::write(fd_, resp.constData(), resp.size());
        }
    }
    // NOLINTEND(readability-identifier-naming)

  private:
    int fd_;
    QSocketNotifier *notifier_;
    QByteArray rx_;
};

// Runs a MockOpenPort on its own thread with its own event loop, so it
// responds like a real adapter regardless of whether the code under test
// pumps events. Responding starts before the constructor returns.
class MockOpenPortThread : public QThread
{
  public:
    explicit MockOpenPortThread(int master_fd) : fd_(master_fd)
    {
        start();
        ready_.acquire();
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
        MockOpenPort m(fd_); // created here => notifier lives on this thread
        mock = &m;
        ready_.release();
        exec();
        mock = nullptr;
    }

  private:
    int fd_;
    QSemaphore ready_;
};
