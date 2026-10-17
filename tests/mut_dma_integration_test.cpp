#include "src/platform/desktop/common/testing/core_application_environment.h"
// Integration tests: the MUT/DMA host client driven through FastECU's REAL serial
// stack against a scripted mock Openport 2.0 over a pseudo-terminal (PTY).
//
//   MutDmaDriver / FastEcuKlineTransport      (code under test, the adapter seam)
//        -> SerialPortActions      (facade, direct mode)
//        -> SerialPortActionsDirect (J2534/Openport branch)
//        -> J2534 (J2534_unix)      (Tactrix serial protocol)
//        -> QSerialPort (PTY slave) <==> MockOpenPort (PTY master)
//
// The unit suite (the //tests Bazel targets) covers the protocol core via ScriptedKlineTransport,
// but explicitly leaves the FastEcuKlineTransport adapter out of the test target
// (see the plan: "the adapter is not part of the test target") because it pulls in
// the whole SerialPortActions facade -- only buildable once the macOS build landed.
// These tests close that gap: they prove the adapter really carries MUT/DMA bytes
// both ways over the production serial path, with no hardware.
//
// The Tactrix wire framing the mock speaks is read straight from J2534_unix.cpp:
//   - host write of a data message:  "att<chan> <size> <flags>\r\n" + <size> raw bytes
//                                     (J2534::PassThruWriteMsgs)
//   - dongle -> host data delivery:  'a''r''5' <len> <NORM_MSG=0x00> <4B ts> <data...>
//                                     where len = 4(ts)+N(data)+1, parsed by
//                                     J2534::PassThruReadMsgs's '3'..'6' branch.
//   - connect handshake acks:        ata->"ari", ati->"ari <ver>", everything else
//                                     -> generic "aro" (same minimal mock that makes
//                                     init_j2534_connection succeed in the crash suite).

#include <tuple>
#include <gtest/gtest.h>
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <QByteArray>
#include <QVector>
#include <QStringList>
#include <QSocketNotifier>
#include <QThread>
#include <QSemaphore>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>

#if defined(__linux__)
#include <pty.h> // openpty
#else
#include <util.h> // openpty
#endif
#include <unistd.h> // read/write/close

#include "src/platform/desktop/common/serial/direct_serial_backend.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/backend/protocol/mut_dma_driver.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"

using namespace mutdma;
using namespace std::chrono_literals;

namespace
{
// The real local backend, as the desktop app's DirectSerial connection builds it.
std::function<SerialBackend *()> DirectBackend()
{
    return [] { return MakeDirectSerialBackend().release(); };
}
} // namespace

// Scripted mock Openport 2.0 on the master side of a PTY. Buffer-driven (not purely
// line-based) so it can faithfully consume the raw data bytes that trail an "att"
// write header -- those bytes are an arbitrary 51-byte MUT frame and may contain
// 0x0D / 0x0A, so they cannot be parsed as a line.
//
// Runs on its own thread (see MockOpenPortThread below) with its own event loop, so
// its QSocketNotifier fires and it answers the wire protocol regardless of whether
// the test/production thread pumps events. This mirrors the crash suite's
// MockOpenPortThread (tests/mock_openport.h): after Task 2 removed the
// QCoreApplication::processEvents() pumps from the backend's blocking reads, a
// same-thread mock relying on the caller's event loop to dispatch its notifier would
// never see the caller's writes and the connect handshake would hang/fail.
class MockOpenPort final : public QObject
{
  public:
    explicit MockOpenPort(int master_fd, QObject *parent = nullptr)
        : QObject(parent), fd_(master_fd), notifier_(new QSocketNotifier(master_fd, QSocketNotifier::Read, this))
    {
        connect(notifier_, &QSocketNotifier::activated, this, &MockOpenPort::OnReadable);
    }

    // The exact payload bytes of the last host data write ("att" message body).
    QByteArray last_write;
    // Polled from the test thread (event wait) while set from the mock
    // thread's onReadable(), so it needs to be atomic.
    std::atomic<bool> saw_write{false};

    // Drop any buffered/partial input left over from the connect handshake (the
    // ISO9141 filter writes trail un-terminated mask/pattern bytes), so a
    // subsequent "att" data write is parsed from a clean buffer.
    void ResetParser()
    {
        rx_.clear();
        last_write.clear();
        saw_write = false;
        raw_remaining_ = 0;
    }

    // Push a dongle->host data delivery carrying `payload` (a streamed MUT frame),
    // framed as an 'ar5' NORM_MSG so J2534::read_j2534_data returns exactly `payload`.
    void InjectDataFrame(const QByteArray& payload)
    {
        QByteArray f;
        f.append('a');
        f.append('r');
        f.append('5');
        f.append(char(payload.size() + 5)); // len = 4 timestamp + N data + 1
        f.append(char(0x00));               // NORM_MSG
        f.append(4, char(0x00));            // 4-byte timestamp (ignored here)
        f.append(payload);
        EXPECT_EQ(::write(fd_, f.constData(), f.size()), f.size());
    }

  private:
    void OnReadable()
    {
        std::array<char, 512> buf{};
        const ssize_t n = ::read(fd_, buf.data(), buf.size());
        if (n <= 0)
        {
            return;
        }
        rx_.append(buf.data(), static_cast<int>(n));
        Process();
    }

  private:
    void Process()
    {
        for (;;)
        {
            if (raw_remaining_ > 0)
            {
                const int take = static_cast<int>(qMin<qsizetype>(raw_remaining_, rx_.size()));
                last_write.append(rx_.left(take));
                rx_.remove(0, take);
                raw_remaining_ -= take;
                if (raw_remaining_ == 0)
                {
                    saw_write = true;
                }
                else
                {
                    return; // need more raw bytes
                }
                continue;
            }

            const qsizetype nl = rx_.indexOf('\n');
            if (nl < 0)
            {
                return;
            }
            const QByteArray line = rx_.left(nl).trimmed();
            rx_.remove(0, nl + 1);
            if (line.isEmpty())
            {
                continue;
            }
            HandleLine(line);
        }
    }

    void HandleLine(const QByteArray& line)
    {
        if (line.startsWith("att"))
        {
            // "att<chan> <size> <flags>" -> consume <size> raw data bytes next.
            const QList<QByteArray> tok = line.split(' ');
            const int size = (tok.size() > 1) ? tok.at(1).toInt() : 0;
            last_write.clear();
            raw_remaining_ = size;
            return; // a data write is not acked by the dongle
        }
        if (line.startsWith("ati"))
        {
            Reply("ari 1.17.4877\r\n");
        }
        else if (line.startsWith("ata"))
        {
            Reply("ari\r\n");
        }
        else
        {
            Reply("aro\r\n"); // generic ack
        }
    }

    void Reply(const char *s)
    {
        const auto length = static_cast<ssize_t>(qstrlen(s));
        EXPECT_EQ(::write(fd_, s, qstrlen(s)), length);
    }

    int fd_;
    QSocketNotifier *notifier_;
    QByteArray rx_;
    int raw_remaining_ = 0;
};

// Runs a MockOpenPort on its own thread with its own event loop, so it responds
// like a real adapter regardless of whether the code under test pumps events.
// Responding starts before the constructor returns. Mirrors
// tests/mock_openport.h's MockOpenPortThread (crash suite).
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

    // Safe to call from the test thread: sawWrite is atomic, and the tests only
    // touch the other (non-atomic) members after synchronizing via sawWrite or
    // with enough of a timing gap (event processing) that the mock thread is idle.
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

class MutDmaIntegrationTest : public ::testing::Test
{

  public:
  protected:
    // Build a SerialPortActions facade (direct mode) connected to `mock` over the
    // PTY whose slave is `name`. Returns the opened-port string ("" on failure).
    static QString ConnectFacade(SerialPortActions& spad, const QString& name)
    {
        spad.SetSerialPortPrefixLinux(""); // PTY name is already absolute
        spad.SetAddSsmHeader(false);
        spad.SetAddIso9141Header(false);
        spad.SetAddIso14230Header(false);
        // open_serial_port() only takes the Openport/J2534 branch when the port
        // description contains "OpenPort 2.0" (J2534_unix.cpp), so tag it as such.
        spad.SetSerialPortList(QStringList() << (name + " - OpenPort 2.0"));
        return spad.OpenSerialPort();
    }
};

TEST_F(MutDmaIntegrationTest, connectsOverMockPty_facadeReportsOpen)
{
    int master = -1, slave = -1;
    std::array<char, 256> name{};
    ASSERT_TRUE(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0) << "openpty failed";
    {
        MockOpenPortThread mock_thread(master);

        SerialPortActions spad{DirectBackend()}; // the real direct backend
        const QString opened = ConnectFacade(spad, QString::fromLocal8Bit(name.data()));

        ASSERT_TRUE(!opened.isEmpty()) << "facade open_serial_port() returned empty";
        ASSERT_TRUE(spad.IsSerialPortOpen());
        ASSERT_TRUE(spad.GetUseOpenport2Adapter());
    }
    ::close(master);
}

TEST_F(MutDmaIntegrationTest, setBaud_throughAdapter_trueWhenConnected_falseWhenClosed)
{
    SerialPortActions closed{DirectBackend()}; // never opened
    FastEcuKlineTransport closed_tr(&closed);
    // change_port_speed returns STATUS_ERROR when the port is not open -> false.
    const auto closed_result = closed_tr.SetBaud(15625);
    ASSERT_TRUE(!closed_result);
    ASSERT_EQ(closed_result.error().kind, fastecu::ErrorKind::kDisconnected);

    int master = -1, slave = -1;
    std::array<char, 256> name{};
    ASSERT_TRUE(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0) << "openpty failed";
    {
        MockOpenPortThread mock_thread(master);

        SerialPortActions spad{DirectBackend()};
        ASSERT_TRUE(!ConnectFacade(spad, QString::fromLocal8Bit(name.data())).isEmpty()) << "connect failed";

        FastEcuKlineTransport tr(&spad);
        // Connected + Openport branch -> change_port_speed routes to SET_CONFIG ioctl
        // and returns STATUS_SUCCESS -> adapter setBaud() == true.
        ASSERT_TRUE(tr.SetBaud(15625));
        ASSERT_TRUE(tr.SetBaud(62500));
    }
    ::close(master);
}

TEST_F(MutDmaIntegrationTest, write_throughAdapter_putsExactFrameOnWire)
{
    int master = -1, slave = -1;
    std::array<char, 256> name{};
    ASSERT_TRUE(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0) << "openpty failed";
    {
        MockOpenPortThread mock_thread(master);
        MockOpenPort& mock = *mock_thread.mock;

        SerialPortActions spad{DirectBackend()};
        ASSERT_TRUE(!ConnectFacade(spad, QString::fromLocal8Bit(name.data())).isEmpty()) << "connect failed";

        FastEcuKlineTransport tr(&spad);

        // A real 0x87 sub-cmd-3 RAM-write frame (51 bytes, contains a 0x0D trailer).
        QByteArray payload;
        payload.append(char(0x00));
        payload.append(char(0x03)); // sub-cmd 3
        payload.append(char(0x12));
        payload.append(char(0x34)); // addr16 (l_command word)
        const QByteArray frame = bytes::ToQByteArray(BuildCommandFrame(0x87, bytes::View(payload), kTrailerStd));
        ASSERT_EQ(frame.size(), kFrameLen);
        ASSERT_TRUE(VerifyFrame(bytes::View(frame)));

        fastecu::testing::ProcessEventsFor(
            std::chrono::milliseconds(100)); // let all connect-handshake bytes reach the mock
        mock.ResetParser();                  // then start capture from a clean buffer

        const auto written = tr.Write(bytes::View(frame));
        ASSERT_TRUE(written);
        ASSERT_EQ(*written, static_cast<std::size_t>(frame.size()));

        // Pump the event loop so the mock's QSocketNotifier drains and captures it.
        ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return bool(mock.saw_write); }, std::chrono::milliseconds(1000)));
        ASSERT_EQ(mock.last_write, frame); // exact bytes, including the 0x0D trailer
    }
    ::close(master);
}

TEST_F(MutDmaIntegrationTest, read_throughAdapter_returnsEcuReplyBytes)
{
    int master = -1, slave = -1;
    std::array<char, 256> name{};
    ASSERT_TRUE(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0) << "openpty failed";
    {
        MockOpenPortThread mock_thread(master);
        MockOpenPort& mock = *mock_thread.mock;

        SerialPortActions spad{DirectBackend()};
        ASSERT_TRUE(!ConnectFacade(spad, QString::fromLocal8Bit(name.data())).isEmpty()) << "connect failed";

        FastEcuKlineTransport tr(&spad);
        fastecu::FakeCancellationToken cancellation;
        std::ignore = tr.Read(60ms, cancellation); // drain any residual init acks before the scripted exchange

        QByteArray reply;
        reply.append(char(0x05));
        reply.append(char(0xAA));
        reply.append(char(0xBB));
        reply.append(char(0xCC));
        reply.append(char(0xDD));
        mock.InjectDataFrame(reply);

        const auto read = tr.Read(500ms, cancellation);
        ASSERT_TRUE(read);
        ASSERT_TRUE(read->has_value());
        ASSERT_EQ(bytes::ToQByteArray(read->value()), reply);
    }
    ::close(master);
}

TEST_F(MutDmaIntegrationTest, driverPollOnce_throughAdapter_decodesStreamFrameFromWire)
{
    int master = -1, slave = -1;
    std::array<char, 256> name{};
    ASSERT_TRUE(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0) << "openpty failed";
    {
        MockOpenPortThread mock_thread(master);
        MockOpenPort& mock = *mock_thread.mock;

        SerialPortActions spad{DirectBackend()};
        ASSERT_TRUE(!ConnectFacade(spad, QString::fromLocal8Bit(name.data())).isEmpty()) << "connect failed";

        FastEcuKlineTransport tr(&spad);
        fastecu::FakeCancellationToken cancellation;
        AlreadyInMode init(125000);
        MutDmaDriver driver(tr, init);

        // Two channels: one 1-byte, one 2-byte, little-endian.
        std::vector<Channel> channels{{0x1234, 1}, {0x5678, 2}};
        driver.SetChannelsForTest(channels);

        // Streamed frame: [logId][data...][sum8(0..len-3)][0x0D].
        const bytes::Byte log_id = 0x00;
        QByteArray data;
        data.append(char(0x42)); // channel 0 (1B) = 0x42
        data.append(char(0xAD));
        data.append(char(0xDE)); // channel 1 (2B) = 0xDEAD
        QByteArray frame;
        frame.append(char(log_id));
        frame.append(data);
        frame.append(char(Sum8(bytes::View(frame))));
        frame.append(char(kTrailerStd));

        std::ignore = tr.Read(60ms, cancellation); // drain residual
        mock.InjectDataFrame(frame);

        const auto values = driver.PollOnce(500ms, cancellation);
        ASSERT_TRUE(values);
        ASSERT_EQ(values->size(), std::size_t(2));
        ASSERT_EQ(values->at(0), std::uint32_t(0x42));
        ASSERT_EQ(values->at(1), std::uint32_t(0xDEAD));
    }
    ::close(master);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
