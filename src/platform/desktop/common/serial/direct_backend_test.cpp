
#include <cstdio>

#include <QCoreApplication>
#include <QtTest>

#include "serial_backend.h"
#include "src/platform/desktop/common/serial/j2534_driver_selection.h"
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

class CheckedJ2534DirectHarness final : public SerialPortActionsDirect
{
  public:
    J2534RawCanOpenResult establish()
    {
        return establish_j2534_raw_can_channel_checked();
    }

    void reset_connection() override
    {
        ++cleanup_calls;
    }

    long connect_result = STATUS_NOERROR;
    long timing_result = STATUS_NOERROR;
    long filter_result = STATUS_NOERROR;
    int connect_calls = 0;
    int timing_calls = 0;
    int filter_calls = 0;
    int cleanup_calls = 0;

  protected:
    long connect_j2534_raw_can_channel() override
    {
        ++connect_calls;
        return connect_result;
    }
    long configure_j2534_raw_can_timings() override
    {
        ++timing_calls;
        return timing_result;
    }
    long configure_j2534_raw_can_filter() override
    {
        ++filter_calls;
        return filter_result;
    }
};

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
    void j2534CapableEntry_matchesOnlyTheAdapterDescription();
    void checkedJ2534RawCanOpen_propagatesConnectTimingAndFilterFailures_data();
    void checkedJ2534RawCanOpen_propagatesConnectTimingAndFilterFailures();
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

    QCOMPARE(resolveJ2534DllForConnection(vendor, dllPath, QStringList() << vendor), dllPath);
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

// The predicate open_serial_port() branches on, and that
// open_desktop_can_flash_transport() selects with. Pinned here so the two
// cannot drift apart (issue #243).
void TestDirectBackend::j2534CapableEntry_matchesOnlyTheAdapterDescription()
{
    QVERIFY(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
    QVERIFY(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0")); // description match is case-insensitive
#if defined Q_OS_UNIX
    // macOS enumerates these ahead of the adapter; driving ISO-15765 over one
    // yields a timeout per exchange, never a response.
    QVERIFY(!isJ2534CapableEntry(u"cu.Bluetooth-Incoming-Port - "));
    QVERIFY(!isJ2534CapableEntry(u"cu.debug-console - "));
    QVERIFY(!isJ2534CapableEntry(u"ttyUSB0 - USB Serial"));
    QVERIFY(!isJ2534CapableEntry(u"ttyUSB0")); // no separator at all
    QVERIFY(!isJ2534CapableEntry(u""));
#else
    // Windows entries come from getAllJ2534DriversNames(), carry no
    // description, and open_serial_port() drives every one of them through
    // J2534 -- rejecting them here would regress Windows.
    QVERIFY(isJ2534CapableEntry(u"Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
    QVERIFY(isJ2534CapableEntry(u"Acme J2534 DLL"));
    QVERIFY(!isJ2534CapableEntry(u""));
#endif
}

void TestDirectBackend::checkedJ2534RawCanOpen_propagatesConnectTimingAndFilterFailures_data()
{
    QTest::addColumn<long>("connect_result");
    QTest::addColumn<long>("timing_result");
    QTest::addColumn<long>("filter_result");
    QTest::addColumn<J2534RawCanOpenFailure>("failure");
    QTest::addColumn<J2534RawCanOpenStage>("stage");
    QTest::addColumn<int>("connect_calls");
    QTest::addColumn<int>("timing_calls");
    QTest::addColumn<int>("filter_calls");

    QTest::newRow("unsupported connect") << static_cast<long>(ERR_INVALID_BAUDRATE) << static_cast<long>(STATUS_NOERROR)
                                         << static_cast<long>(STATUS_NOERROR)
                                         << J2534RawCanOpenFailure::UnsupportedConfiguration
                                         << J2534RawCanOpenStage::ChannelConnect << 1 << 0 << 0;
    QTest::newRow("internal timing") << static_cast<long>(STATUS_NOERROR) << static_cast<long>(ERR_FAILED)
                                     << static_cast<long>(STATUS_NOERROR) << J2534RawCanOpenFailure::Internal
                                     << J2534RawCanOpenStage::TimingConfiguration << 1 << 1 << 0;
    QTest::newRow("disconnected filter") << static_cast<long>(STATUS_NOERROR) << static_cast<long>(STATUS_NOERROR)
                                         << static_cast<long>(ERR_DEVICE_NOT_CONNECTED)
                                         << J2534RawCanOpenFailure::AdapterUnavailable
                                         << J2534RawCanOpenStage::FilterConfiguration << 1 << 1 << 1;
}

void TestDirectBackend::checkedJ2534RawCanOpen_propagatesConnectTimingAndFilterFailures()
{
    QFETCH(long, connect_result);
    QFETCH(long, timing_result);
    QFETCH(long, filter_result);
    QFETCH(J2534RawCanOpenFailure, failure);
    QFETCH(J2534RawCanOpenStage, stage);
    QFETCH(int, connect_calls);
    QFETCH(int, timing_calls);
    QFETCH(int, filter_calls);
    CheckedJ2534DirectHarness harness;
    harness.connect_result = connect_result;
    harness.timing_result = timing_result;
    harness.filter_result = filter_result;

    const auto result = harness.establish();

    QCOMPARE(result.failure, failure);
    QCOMPARE(result.stage, stage);
    QCOMPARE(result.api_status, stage == J2534RawCanOpenStage::ChannelConnect        ? connect_result
                                : stage == J2534RawCanOpenStage::TimingConfiguration ? timing_result
                                                                                     : filter_result);
    QCOMPARE(harness.connect_calls, connect_calls);
    QCOMPARE(harness.timing_calls, timing_calls);
    QCOMPARE(harness.filter_calls, filter_calls);
    QCOMPARE(harness.cleanup_calls, 1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    TestDirectBackend test;
    return QTest::qExec(&test, argc, argv);
}

#include "direct_backend_test.moc"
