#include <QtTest>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::append_j2534_interfaces;
    using SerialPortActionsDirect::resolve_port;
};

class TestDirectBackendHooksUnix : public QObject
{
    Q_OBJECT
  private slots:
    void resolvePort_prefixesAndSplitsAnAdapterEntry()
    {
        HookProbe probe;
        const auto resolved = probe.resolve_port("cu.usbmodem0 - OpenPort 2.0");
        QCOMPARE(resolved.port, QString("/dev/cu.usbmodem0"));
        QVERIFY(resolved.is_j2534);
    }

    // macOS lists this port ahead of the adapter; it must fall back to plain serial.
    void resolvePort_plainSerialEntryIsNotJ2534()
    {
        HookProbe probe;
        const auto bluetooth = probe.resolve_port("cu.Bluetooth-Incoming-Port - ");
        QCOMPARE(bluetooth.port, QString("/dev/cu.Bluetooth-Incoming-Port"));
        QVERIFY(!bluetooth.is_j2534);
        const auto usb = probe.resolve_port("ttyUSB0 - USB Serial");
        QCOMPARE(usb.port, QString("/dev/ttyUSB0"));
        QVERIFY(!usb.is_j2534);
    }

    void appendJ2534Interfaces_leavesTheListUntouched()
    {
        HookProbe probe;
        QStringList ports{"cu.usbmodem0 - OpenPort 2.0"};
        probe.append_j2534_interfaces(ports);
        QCOMPARE(ports, QStringList{"cu.usbmodem0 - OpenPort 2.0"});
    }
};

QTEST_GUILESS_MAIN(TestDirectBackendHooksUnix)
#include "direct_backend_hooks_unix_test.moc"
