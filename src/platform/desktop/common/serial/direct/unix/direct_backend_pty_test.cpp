#include <QThread>
#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <array>
#include <cstdio>

#include <QCoreApplication>
#include <QSerialPort>
#include <gtest/gtest.h>
#include <thread>

#if defined(__linux__)
#include <pty.h> // openpty
#else
#include <util.h> // openpty
#endif
#include <poll.h>
#include <unistd.h>

#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

// Backend behavior over a PTY (spec: reassembly, timeout, buffer clearing,
// adapter-vanish). J2534 ioctl parameter handling stays with the crash suite's
// MockOpenPort + the bench checklist.
class TestDirectBackendPty : public ::testing::Test
{

  public:
    static void SetUpTestSuite();

  protected:
    int openPtyBackend(SerialPortActionsDirect& backend);
};

void TestDirectBackendPty::SetUpTestSuite()
{
    ASSERT_TRUE(QCoreApplication::instance());
}

int TestDirectBackendPty::openPtyBackend(SerialPortActionsDirect& backend)
{
    int master = -1, slave = -1;
    std::array<char, 256> name{};
    if (openpty(&master, &slave, name.data(), nullptr, nullptr) != 0)
    {
        return -1;
    }
    backend.serial_port_prefix_linux = "";
    backend.serial_port_list = QStringList() << QString::fromLocal8Bit(name.data());
    if (backend.open_serial_port() != QString::fromLocal8Bit(name.data()))
    {
        return -1;
    }
    return master;
}

TEST_F(TestDirectBackendPty, ptyRead_reassemblesFragmentedFrame)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);

    // The "ECU" delivers one framed message in two fragments with a gap:
    // header first, payload+checksum 30ms later. The reader must reassemble.
    std::thread responder(
        [master]
        {
            EXPECT_EQ(::write(master, "\x80\xf0\x10\x02", 4), 4);
            QThread::msleep(30);
            EXPECT_EQ(::write(master, "\xaa\xbb\xcc", 3), 3);
        });
    const QByteArray got = direct.read_serial_data(500);
    responder.join();
    ASSERT_EQ(got, QByteArray("\x80\xf0\x10\x02\xaa\xbb\xcc", 7));
    ::close(master);
}

TEST_F(TestDirectBackendPty, ptyRead_timesOutCleanOnSilence)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);

    QElapsedTimer t;
    t.start();
    ASSERT_EQ(direct.read_serial_data(150), QByteArray());
    const qint64 elapsed = t.elapsed();
    ASSERT_TRUE(elapsed >= 140 && elapsed < 1000) << qPrintable(QString("timeout took %1 ms").arg(elapsed));
    ::close(master);
}

TEST_F(TestDirectBackendPty, ptyClearRxBuffer_discardsPendingBytes)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);

    ASSERT_EQ(::write(master, "\x11\x22\x33", 3), 3); // junk arrives...
    QThread::msleep(50);                              // ...and lands in the buffer
    direct.clear_rx_buffer();                         // must discard it
    ASSERT_EQ(direct.read_serial_data(100), QByteArray());
    ::close(master);
}

TEST_F(TestDirectBackendPty, ptyAdapterVanish_readReturnsCleanly)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);

    ::close(master); // the adapter disappears
    // The read must come back empty (possibly via handle_error ->
    // reset_connection) without crashing or hanging.
    ASSERT_EQ(direct.read_serial_data(100), QByteArray());
}

TEST_F(TestDirectBackendPty, ptyParityChangesWhileOpen)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);
    ASSERT_TRUE(direct.set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)));
    ASSERT_EQ(direct.get_serial_port_parity(), static_cast<std::uint8_t>(QSerialPort::NoParity));

    const bool evenParitySet = direct.set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::EvenParity));
#if defined(__linux__)
    // Linux PTYs have no parity hardware and may reject PARENB. Keep testing
    // the open-port NoParity path when this PTY cannot accept even parity.
    if (!evenParitySet)
    {
        ASSERT_TRUE(direct.set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)));
        ASSERT_EQ(direct.get_serial_port_parity(), static_cast<std::uint8_t>(QSerialPort::NoParity));
        ::close(master);
        return;
    }
#endif
    ASSERT_TRUE(evenParitySet);
    ASSERT_EQ(direct.get_serial_port_parity(), static_cast<std::uint8_t>(QSerialPort::EvenParity));
    ASSERT_TRUE(direct.set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)));
    ASSERT_EQ(direct.get_serial_port_parity(), static_cast<std::uint8_t>(QSerialPort::NoParity));
    ::close(master);
}

namespace
{
// Collects what the backend wrote to the PTY until `count` bytes arrive or
// the deadline passes. QSerialPort flushes from the event loop, so events are
// pumped while waiting.
QByteArray readFromMaster(int master, qsizetype count)
{
    QByteArray got;
    QElapsedTimer t;
    t.start();
    while (got.size() < count && t.elapsed() < 1000)
    {
        QCoreApplication::processEvents();
        pollfd fd{.fd = master, .events = POLLIN, .revents = 0};
        if (::poll(&fd, 1, 10) > 0)
        {
            std::array<char, 64> buffer{};
            const ssize_t n = ::read(master, buffer.data(), buffer.size());
            if (n > 0)
            {
                got.append(buffer.data(), n);
            }
        }
    }
    return got;
}
} // namespace

// A payload shorter than 0x40 has its length ORed into the start byte. The
// start byte's top bit is set, so the folded byte is negative as a char.
TEST_F(TestDirectBackendPty, ptyIso14230Write_foldsShortLengthIntoFormatByte)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);
    direct.set_add_iso14230_header(true);
    direct.set_kline_startbyte(0x80);
    direct.set_kline_target_id(0x10);
    direct.set_kline_tester_id(0xf1);

    direct.write_serial_data(QByteArray("\x21\x81", 2));

    // Checksum: 0x82 + 0x10 + 0xf1 + 0x21 + 0x81 = 0x225 -> 0x25.
    ASSERT_EQ(readFromMaster(master, 6), QByteArray("\x82\x10\xf1\x21\x81\x25", 6));
    ::close(master);
}

// The format byte's low six bits give the payload length; the header's fourth
// byte already belongs to the payload. A trailing byte past that length stays
// unread.
TEST_F(TestDirectBackendPty, ptyIso14230Read_takesLengthFromFormatByte)
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    ASSERT_TRUE(master >= 0);
    direct.set_is_iso14230_connection(true);

    ASSERT_EQ(::write(master, "\x82\x10\xf1\xaa\xbb\xcc\xdd", 7), 7);
    ASSERT_EQ(direct.read_serial_data(500), QByteArray("\x82\x10\xf1\xaa\xbb\xcc", 6));
    ::close(master);
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment(
        []
        {
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }));
} // namespace
