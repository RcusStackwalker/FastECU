#include "src/platform/desktop/common/testing/core_application_environment.h"
// Unit tests for the transport factory. Exists so //apps/bench can obtain an
// ICanFlashTransport without naming SerialPortActions, whose target is
// platform-only.
#include "src/platform/desktop/common/transport/desktop_transport_factory.h"

#include <gtest/gtest.h>
#include <QCoreApplication>

#include <gmock/gmock.h>

#include <memory>

#include "src/platform/desktop/common/serial/testing/fake_backend.h"

using fastecu::ErrorKind;
using fastecu::flash::DesktopCanTransportConfig;
using fastecu::flash::Iso15765Config;
using fastecu::flash::ListDesktopSerialPorts;
using fastecu::flash::OpenDesktopCanFlashTransport;

namespace
{
constexpr Iso15765Config kColtCan{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false};

// check_serial_ports() entries are "<portName> - <description>". Only the
// description identifies a J2534 adapter; a plain serial port carries an empty
// one, and macOS sorts exactly such a port ahead of the adapter.
const QString kOpenPort0 = "cu.usbmodem0 - OpenPort 2.0";
const QString kOpenPort1 = "cu.usbmodem1 - OpenPort 2.0";
const QString kBluetoothPort = "cu.Bluetooth-Incoming-Port - ";

DesktopCanTransportConfig ConfigWith(FakeBackend **captured, QStringList ports, QString open_result)
{
    DesktopCanTransportConfig config;
    config.backend_factory = [captured, ports, open_result]() -> SerialBackend *
    {
        auto *fake = new NiceFakeBackend();
        EXPECT_CALL(*fake, CheckSerialPorts()).WillRepeatedly(::testing::Return(ports));
        EXPECT_CALL(*fake, OpenSerialPort()).WillRepeatedly(::testing::Return(open_result));
        *captured = fake;
        return fake;
    };
    return config;
}
} // namespace

TEST(TestDesktopTransportFactory, listsEveryDetectedPort)
{
    FakeBackend *fake = nullptr;
    const auto ports = ListDesktopSerialPorts(ConfigWith(&fake, {kOpenPort0, kOpenPort1}, ""));

    ASSERT_TRUE(ports.has_value());
    ASSERT_EQ(ports->size(), 2U);
    ASSERT_EQ(QString::fromStdString((*ports)[1]), kOpenPort1);
}

TEST(TestDesktopTransportFactory, refusesToOpenWhenNoDeviceIsDetected)
{
    FakeBackend *fake = nullptr;
    const auto transport = OpenDesktopCanFlashTransport(ConfigWith(&fake, {}, kOpenPort0), kColtCan);

    ASSERT_TRUE(!transport.has_value());
    ASSERT_EQ(transport.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopTransportFactory, refusesToOpenWhenTheNamedDeviceIsAbsent)
{
    FakeBackend *fake = nullptr;
    auto config = ConfigWith(&fake, {kOpenPort0}, kOpenPort0);
    config.port_name = "cu.usbmodem7 - OpenPort 2.0";
    const auto transport = OpenDesktopCanFlashTransport(config, kColtCan);

    ASSERT_TRUE(!transport.has_value());
    ASSERT_EQ(transport.error().kind, ErrorKind::kInvalidConfig);
}

TEST(TestDesktopTransportFactory, selectsTheFirstJ2534DeviceWhenNoNameIsGiven)
{
    FakeBackend *fake = nullptr;
    const auto transport =
        OpenDesktopCanFlashTransport(ConfigWith(&fake, {kOpenPort0, kOpenPort1}, kOpenPort0), kColtCan);

    ASSERT_TRUE(transport.has_value());
    ASSERT_EQ(fake->GetSerialPortList(), QStringList({kOpenPort0}));
}

// Regression (issue #243): QSerialPortInfo sorts
// "cu.Bluetooth-Incoming-Port - " ahead of the adapter on macOS, so taking
// detected.front() landed on a port that open_serial_port() never drives
// through J2534 -- it silently degrades to a plain serial port, reports
// success, and every ISO-15765 exchange then times out with no response.
TEST(TestDesktopTransportFactory, skipsNonJ2534PortsWhenNoNameIsGiven)
{
    FakeBackend *fake = nullptr;
    const auto transport =
        OpenDesktopCanFlashTransport(ConfigWith(&fake, {kBluetoothPort, kOpenPort0}, kOpenPort0), kColtCan);

    ASSERT_TRUE(transport.has_value());
#if defined(Q_OS_UNIX)
    ASSERT_EQ(fake->GetSerialPortList(), QStringList({kOpenPort0}));
#else
    // Windows entries come from the J2534 driver registry rather than the
    // serial-port list, so every one of them is adapter-capable and the
    // first is still the right choice.
    ASSERT_EQ(fake->GetSerialPortList(), QStringList({kBluetoothPort}));
#endif
}

// The guards below sit inside the slot bodies, not around the slot
// declarations: moc does not evaluate Q_OS_UNIX, so a guarded declaration
// compiles but never reaches the meta-object and the test silently never
// runs.
TEST(TestDesktopTransportFactory, refusesToOpenWhenNoDetectedPortIsAJ2534Adapter)
{
    FakeBackend *fake = nullptr;
    const auto transport = OpenDesktopCanFlashTransport(ConfigWith(&fake, {kBluetoothPort}, kBluetoothPort), kColtCan);

#if defined(Q_OS_UNIX)
    ASSERT_TRUE(!transport.has_value());
    ASSERT_EQ(transport.error().kind, ErrorKind::kDisconnected);
#else
    ASSERT_TRUE(transport.has_value());
#endif
}

// Naming the dead port explicitly must fail loudly for the same reason:
// ISO-15765 cannot run over a plain serial port, so accepting it only buys
// one read timeout per exchange.
TEST(TestDesktopTransportFactory, refusesToOpenWhenTheNamedDeviceIsNotAJ2534Adapter)
{
    FakeBackend *fake = nullptr;
    auto config = ConfigWith(&fake, {kBluetoothPort, kOpenPort0}, kBluetoothPort);
    config.port_name = kBluetoothPort.toStdString();
    const auto transport = OpenDesktopCanFlashTransport(config, kColtCan);

#if defined(Q_OS_UNIX)
    ASSERT_TRUE(!transport.has_value());
    ASSERT_EQ(transport.error().kind, ErrorKind::kInvalidConfig);
#else
    ASSERT_TRUE(transport.has_value());
#endif
}

TEST(TestDesktopTransportFactory, reportsDisconnectedWhenTheOpenFails)
{
    FakeBackend *fake = nullptr;
    const auto transport = OpenDesktopCanFlashTransport(ConfigWith(&fake, {kOpenPort0}, ""), kColtCan);

    ASSERT_TRUE(!transport.has_value());
    ASSERT_EQ(transport.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopTransportFactory, refusesAConfigWithoutABackendFactory)
{
    const DesktopCanTransportConfig config;

    const auto ports = ListDesktopSerialPorts(config);
    ASSERT_TRUE(!ports.has_value());
    ASSERT_EQ(ports.error().kind, ErrorKind::kInvalidConfig);

    const auto transport = OpenDesktopCanFlashTransport(config, kColtCan);
    ASSERT_TRUE(!transport.has_value());
    ASSERT_EQ(transport.error().kind, ErrorKind::kInvalidConfig);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
