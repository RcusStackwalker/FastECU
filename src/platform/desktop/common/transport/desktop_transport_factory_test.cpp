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
        config.port_name = "op2-7 - OpenPort 2.0";
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

    // Regression: QSerialPortInfo sorts "cu.Bluetooth-Incoming-Port - " ahead
    // of the adapter on macOS, so the implicit default used to land on a port
    // that SerialPortActionsDirect::open_serial_port() never drives through
    // J2534 -- it silently falls back to a dumb serial port and every
    // ISO-15765 exchange then times out with no response PDU.
    void skipsNonJ2534PortsWhenNoNameIsGiven()
    {
        FakeBackend *fake = nullptr;
        const auto transport = open_desktop_can_flash_transport(
            configWith(&fake, {kBluetoothPort, kOpenPort0}, kOpenPort0), kColtCan);

        QVERIFY(transport.has_value());
        QCOMPARE(fake->get_serial_port_list(), QStringList({kOpenPort0}));
    }

    void refusesToOpenWhenNoDetectedPortIsAJ2534Adapter()
    {
        FakeBackend *fake = nullptr;
        const auto transport =
            open_desktop_can_flash_transport(configWith(&fake, {kBluetoothPort}, kBluetoothPort), kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::Disconnected);
    }

    // Naming the dead port explicitly must fail loudly for the same reason:
    // ISO-15765 cannot run over a plain serial port, so accepting it only
    // buys a five-second timeout per exchange.
    void refusesToOpenWhenTheNamedDeviceIsNotAJ2534Adapter()
    {
        FakeBackend *fake = nullptr;
        auto config = configWith(&fake, {kBluetoothPort, kOpenPort0}, kBluetoothPort);
        config.port_name = kBluetoothPort.toStdString();
        const auto transport = open_desktop_can_flash_transport(config, kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::InvalidConfig);
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
