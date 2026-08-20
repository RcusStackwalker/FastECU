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
        const auto ports = list_desktop_serial_ports(configWith(&fake, {"op2-0", "op2-1"}, ""));

        QVERIFY(ports.has_value());
        QCOMPARE(ports->size(), 2u);
        QCOMPARE(QString::fromStdString((*ports)[1]), QString("op2-1"));
    }

    void refusesToOpenWhenNoDeviceIsDetected()
    {
        FakeBackend *fake = nullptr;
        const auto transport = open_desktop_can_flash_transport(configWith(&fake, {}, "op2-0"), kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::Disconnected);
    }

    void refusesToOpenWhenTheNamedDeviceIsAbsent()
    {
        FakeBackend *fake = nullptr;
        auto config = configWith(&fake, {"op2-0"}, "op2-0");
        config.port_name = "op2-7";
        const auto transport = open_desktop_can_flash_transport(config, kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::InvalidConfig);
    }

    void selectsTheFirstDeviceWhenNoNameIsGiven()
    {
        FakeBackend *fake = nullptr;
        const auto transport =
            open_desktop_can_flash_transport(configWith(&fake, {"op2-0", "op2-1"}, "op2-0"), kColtCan);

        QVERIFY(transport.has_value());
        QVERIFY(fake->takeCallLog().contains("cfg:set_serial_port:op2-0"));
    }

    void reportsDisconnectedWhenTheOpenFails()
    {
        FakeBackend *fake = nullptr;
        // Empty openSerialPortResult is FakeBackend's failure sentinel.
        const auto transport = open_desktop_can_flash_transport(configWith(&fake, {"op2-0"}, ""), kColtCan);

        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::Disconnected);
    }
};

QTEST_MAIN(TestDesktopTransportFactory)
#include "desktop_transport_factory_test.moc"
