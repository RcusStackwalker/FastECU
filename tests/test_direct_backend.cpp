#include "test_direct_backend.h"

#include <QtTest>

#include "serial_backend.h"
#include "serial_port/j2534_driver_selection.h"
#include "serial_port_actions_direct.h"

// Exercises the SerialBackend contract against the direct implementation
// purely through the base-class pointer: get/set roundtrips hit the same
// storage the backend's own I/O logic reads, and closed-port I/O calls
// return their documented empty/error values without hardware.
class TestDirectBackend : public QObject
{
    Q_OBJECT
  private slots:
    void getSet_roundtrip_throughInterface();
    void closedPort_ioCalls_returnEmpty();
    void j2534Selection_usesInstalledDllPathAfterVendorProbe();
    void j2534DriverViews_wow6432NodeVendorIsDiscoverable();
    void j2534DriverViews_laterViewOverwritesOnCollision();
};

void TestDirectBackend::getSet_roundtrip_throughInterface()
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    b->set_add_ssm_header(true);
    QCOMPARE(b->get_add_ssm_header(), true);
    QCOMPARE(direct.add_ssm_header, true); // same storage the I/O paths read

    b->set_serial_port_baudrate("10400");
    QCOMPARE(b->get_serial_port_baudrate(), QString("10400"));
    QCOMPARE(direct.serial_port_baudrate, QString("10400"));

    b->set_kline_startbyte(0x80);
    QCOMPARE(b->get_kline_startbyte(), (uint8_t)0x80);

    b->set_can_source_address(0x7E0);
    QCOMPARE(b->get_can_source_address(), (uint32_t)0x7E0);

    b->set_serial_port_list(QStringList() << "ttyUSB0");
    QCOMPARE(b->get_serial_port_list(), QStringList() << "ttyUSB0");

    QCOMPARE(b->qobject(), static_cast<QObject *>(&direct));
}

void TestDirectBackend::closedPort_ioCalls_returnEmpty()
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    QCOMPARE(b->is_serial_port_open(), false);
    QCOMPARE(b->read_serial_data(50), QByteArray());
    // write_serial_data's `return STATUS_SUCCESS;` converts int 0 through the
    // QByteArray(const char*) ctor => empty array. Pin today's behavior.
    QCOMPARE(b->write_serial_data(QByteArray("\x01\x02", 2)), QByteArray());
    b->waitForSource(); // default no-op must not block or crash
}

void TestDirectBackend::j2534Selection_usesInstalledDllPathAfterVendorProbe()
{
    const QString vendor = "Tactrix Inc. - OpenPort 2.0 J2534 DLL";
    const QString dllPath = "C:\\Program Files (x86)\\OpenECU\\OpenPort 2.0\\op20pt32.dll";

    QCOMPARE(resolveJ2534DllForConnection(vendor, dllPath, QStringList() << vendor),
             dllPath);
}

void TestDirectBackend::j2534DriverViews_wow6432NodeVendorIsDiscoverable()
{
    QMap<QString, QString> nativeView;
    nativeView["Tactrix Inc. - OpenPort 2.0 J2534 DLL"] = "C:\\Program Files\\OpenECU\\OpenPort 2.0\\op20pt32.dll";

    QMap<QString, QString> wow64View;
    wow64View["Acme 32-bit-only J2534 DLL"] = "C:\\Program Files (x86)\\Acme\\acme_j2534.dll";

    QMap<QString, QString> merged = mergeJ2534DriverViews(wow64View, nativeView);

    QCOMPARE(merged.size(), 2);
    QCOMPARE(merged.value("Tactrix Inc. - OpenPort 2.0 J2534 DLL"),
             QString("C:\\Program Files\\OpenECU\\OpenPort 2.0\\op20pt32.dll"));
    QCOMPARE(merged.value("Acme 32-bit-only J2534 DLL"), QString("C:\\Program Files (x86)\\Acme\\acme_j2534.dll"));
}

void TestDirectBackend::j2534DriverViews_laterViewOverwritesOnCollision()
{
    QMap<QString, QString> wow64View;
    wow64View["Shared Vendor"] = "C:\\wow64\\path.dll";

    QMap<QString, QString> nativeView;
    nativeView["Shared Vendor"] = "C:\\native\\path.dll";

    QMap<QString, QString> merged = mergeJ2534DriverViews(wow64View, nativeView);

    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.value("Shared Vendor"), QString("C:\\native\\path.dll"));
}

int run_test_direct_backend(int argc, char **argv)
{
    TestDirectBackend t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_direct_backend.moc"
