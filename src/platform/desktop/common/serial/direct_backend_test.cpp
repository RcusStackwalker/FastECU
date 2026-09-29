#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <cstdio>
#include <memory>

#include <QCoreApplication>
#include <gtest/gtest.h>

#include "serial_backend.h"
#include "src/platform/desktop/common/serial/direct_serial_backend.h"
#include "src/platform/desktop/common/serial/j2534_driver_selection.h"
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exercises the SerialBackend contract against the direct implementation
// purely through the base-class pointer: get/set roundtrips hit the same
// storage the backend's own I/O logic reads, and closed-port I/O calls
// return their documented empty/error values without hardware.
class TestDirectBackend : public ::testing::Test
{

  public:
};

TEST_F(TestDirectBackend, getSet_roundtrip_throughInterface)
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    b->set_add_ssm_header(true);
    ASSERT_EQ(b->get_add_ssm_header(), true);
    ASSERT_EQ(direct.add_ssm_header, true); // same storage the I/O paths read

    b->set_serial_port_baudrate("10400");
    ASSERT_EQ(b->get_serial_port_baudrate(), QString("10400"));
    ASSERT_EQ(direct.serial_port_baudrate, QString("10400"));

    b->set_kline_startbyte(0x80);
    ASSERT_EQ(b->get_kline_startbyte(), (uint8_t)0x80);

    b->set_can_source_address(0x7E0);
    ASSERT_EQ(b->get_can_source_address(), (uint32_t)0x7E0);

    b->set_serial_port_list(QStringList() << "ttyUSB0");
    ASSERT_EQ(b->get_serial_port_list(), QStringList() << "ttyUSB0");

    ASSERT_EQ(b->qobject(), static_cast<QObject *>(&direct));
}

TEST_F(TestDirectBackend, closedPort_ioCalls_returnEmpty)
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    ASSERT_EQ(b->is_serial_port_open(), false);
    ASSERT_EQ(b->read_serial_data(50), QByteArray());
    // write_serial_data's `return STATUS_SUCCESS;` converts int 0 through the
    // QByteArray(const char*) ctor => empty array. Pin today's behavior.
    ASSERT_EQ(b->write_serial_data(QByteArray("\x01\x02", 2)), QByteArray());
    b->waitForSource(); // default no-op must not block or crash
}

TEST_F(TestDirectBackend, j2534Selection_usesInstalledDllPathAfterVendorProbe)
{
    const QString vendor = "Tactrix Inc. - OpenPort 2.0 J2534 DLL";
    const QString dllPath = "C:\\Program Files (x86)\\OpenECU\\OpenPort 2.0\\op20pt32.dll";

    ASSERT_EQ(resolveJ2534DllForConnection(vendor, dllPath, QStringList() << vendor), dllPath);
}

TEST_F(TestDirectBackend, j2534DriverViews_wow6432NodeVendorIsDiscoverable)
{
    QMap<QString, QString> nativeView;
    nativeView["Tactrix Inc. - OpenPort 2.0 J2534 DLL"] = "C:\\Program Files\\OpenECU\\OpenPort 2.0\\op20pt32.dll";

    QMap<QString, QString> wow64View;
    wow64View["Acme 32-bit-only J2534 DLL"] = "C:\\Program Files (x86)\\Acme\\acme_j2534.dll";

    QMap<QString, QString> merged = mergeJ2534DriverViews(wow64View, nativeView);

    ASSERT_EQ(merged.size(), 2);
    ASSERT_EQ(merged.value("Tactrix Inc. - OpenPort 2.0 J2534 DLL"),
              QString("C:\\Program Files\\OpenECU\\OpenPort 2.0\\op20pt32.dll"));
    ASSERT_EQ(merged.value("Acme 32-bit-only J2534 DLL"), QString("C:\\Program Files (x86)\\Acme\\acme_j2534.dll"));
}

TEST_F(TestDirectBackend, j2534DriverViews_laterViewOverwritesOnCollision)
{
    QMap<QString, QString> wow64View;
    wow64View["Shared Vendor"] = "C:\\wow64\\path.dll";

    QMap<QString, QString> nativeView;
    nativeView["Shared Vendor"] = "C:\\native\\path.dll";

    QMap<QString, QString> merged = mergeJ2534DriverViews(wow64View, nativeView);

    ASSERT_EQ(merged.size(), 1);
    ASSERT_EQ(merged.value("Shared Vendor"), QString("C:\\native\\path.dll"));
}

TEST_F(TestDirectBackend, makeDirectSerialBackend_buildsTheDirectBackend)
{
    const std::unique_ptr<SerialBackend> backend = make_direct_serial_backend();
    ASSERT_TRUE(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
    return RUN_ALL_TESTS();
}
