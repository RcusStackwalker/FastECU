// Unit tests for the transport factory. Exists so //apps/bench can obtain an
// ICanFlashTransport without naming SerialPortActions, whose visibility list
// //:serial_compat_allowlist freezes.
#include "src/platform/desktop/common/transport/desktop_transport_factory.h"

#include <QTest>

#include <memory>

#include "src/platform/desktop/common/serial/testing/fake_backend.h"

using fastecu::ErrorKind;
using fastecu::flash::DesktopCanTransportConfig;
using fastecu::flash::Iso15765Config;
using fastecu::flash::list_desktop_serial_ports;
using fastecu::flash::open_desktop_can_flash_transport;

namespace
{
constexpr Iso15765Config kColtCan{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false};

// check_serial_ports() entries are "<portName> - <description>". Only the
// description identifies a J2534 adapter; a plain serial port carries an empty
// one, and macOS sorts exactly such a port ahead of the adapter.
const QString kOpenPort0 = "cu.usbmodem0 - OpenPort 2.0";
const QString kOpenPort1 = "cu.usbmodem1 - OpenPort 2.0";
const QString kBluetoothPort = "cu.Bluetooth-Incoming-Port - ";

DesktopCanTransportConfig configWith(FakeBackend **captured, QStringList ports, QString openResult)
{
    DesktopCanTransportConfig config;
    config.backend_factory_for_tests = [captured, ports, openResult]() -> SerialBackend *
    {
        auto *fake = new FakeBackend();
        fake->checkSerialPortsResult = ports;
        fake->openSerialPortResult = openResult;
        *captured = fake;
        return fake;
    };
    return config;
}
} // namespace

class TestDesktopTransportFactory : public QObject
{
    Q_OBJECT

  private slots:

    void listsEveryDetectedPort()
    {
        FakeBackend *fake = nullptr;
        const auto ports = list_desktop_serial_ports(configWith(&fake, {kOpenPort0, kOpenPort1}, ""));

        QVERIFY(ports.has_value());
        QCOMPARE(ports->size(), 2u);
        QCOMPARE(QString::fromStdString((*ports)[1]), kOpenPort1);
    }

    void refusesToOpenWhenNoDeviceIsDetected()
    {
        FakeBackend *fake = nullptr;
        const auto transport = open_desktop_can_flash_transport(configWith(&fake, {}, kOpenPort0), kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::Disconnected);
    }

    void refusesToOpenWhenTheNamedDeviceIsAbsent()
    {
        FakeBackend *fake = nullptr;
        auto config = configWith(&fake, {kOpenPort0}, kOpenPort0);
        config.port_name = "cu.usbmodem7 - OpenPort 2.0";
        const auto transport = open_desktop_can_flash_transport(config, kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::InvalidConfig);
    }

    void selectsTheFirstJ2534DeviceWhenNoNameIsGiven()
    {
        FakeBackend *fake = nullptr;
        const auto transport =
            open_desktop_can_flash_transport(configWith(&fake, {kOpenPort0, kOpenPort1}, kOpenPort0), kColtCan);

        QVERIFY(transport.has_value());
        QCOMPARE(fake->get_serial_port_list(), QStringList({kOpenPort0}));
    }

    // Regression (issue #243): QSerialPortInfo sorts
    // "cu.Bluetooth-Incoming-Port - " ahead of the adapter on macOS, so taking
    // detected.front() landed on a port that open_serial_port() never drives
    // through J2534 -- it silently degrades to a plain serial port, reports
    // success, and every ISO-15765 exchange then times out with no response.
    void skipsNonJ2534PortsWhenNoNameIsGiven()
    {
        FakeBackend *fake = nullptr;
        const auto transport =
            open_desktop_can_flash_transport(configWith(&fake, {kBluetoothPort, kOpenPort0}, kOpenPort0), kColtCan);

        QVERIFY(transport.has_value());
#if defined(Q_OS_UNIX)
        QCOMPARE(fake->get_serial_port_list(), QStringList({kOpenPort0}));
#else
        // Windows entries come from the J2534 driver registry rather than the
        // serial-port list, so every one of them is adapter-capable and the
        // first is still the right choice.
        QCOMPARE(fake->get_serial_port_list(), QStringList({kBluetoothPort}));
#endif
    }

    // The guards below sit inside the slot bodies, not around the slot
    // declarations: moc does not evaluate Q_OS_UNIX, so a guarded declaration
    // compiles but never reaches the meta-object and the test silently never
    // runs.
    void refusesToOpenWhenNoDetectedPortIsAJ2534Adapter()
    {
        FakeBackend *fake = nullptr;
        const auto transport =
            open_desktop_can_flash_transport(configWith(&fake, {kBluetoothPort}, kBluetoothPort), kColtCan);

#if defined(Q_OS_UNIX)
        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::Disconnected);
#else
        QVERIFY(transport.has_value());
#endif
    }

    // Naming the dead port explicitly must fail loudly for the same reason:
    // ISO-15765 cannot run over a plain serial port, so accepting it only buys
    // one read timeout per exchange.
    void refusesToOpenWhenTheNamedDeviceIsNotAJ2534Adapter()
    {
        FakeBackend *fake = nullptr;
        auto config = configWith(&fake, {kBluetoothPort, kOpenPort0}, kBluetoothPort);
        config.port_name = kBluetoothPort.toStdString();
        const auto transport = open_desktop_can_flash_transport(config, kColtCan);

#if defined(Q_OS_UNIX)
        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::InvalidConfig);
#else
        QVERIFY(transport.has_value());
#endif
    }

    void reportsDisconnectedWhenTheOpenFails()
    {
        FakeBackend *fake = nullptr;
        // Empty openSerialPortResult is FakeBackend's failure sentinel.
        const auto transport = open_desktop_can_flash_transport(configWith(&fake, {kOpenPort0}, ""), kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::Disconnected);
    }
};

QTEST_MAIN(TestDesktopTransportFactory)
#include "desktop_transport_factory_test.moc"
