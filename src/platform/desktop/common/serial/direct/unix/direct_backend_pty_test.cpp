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
            ::write(master, "\x80\xf0\x10\x02", 4);
            QThread::msleep(30);
            ::write(master, "\xaa\xbb\xcc", 3);
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

    ::write(master, "\x11\x22\x33", 3); // junk arrives...
    QThread::msleep(50);                // ...and lands in the buffer
    direct.clear_rx_buffer();           // must discard it
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
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment(
        []
        {
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }));
} // namespace
