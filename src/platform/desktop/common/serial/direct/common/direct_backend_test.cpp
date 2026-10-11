#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <cstdio>
#include <memory>

#include <QCoreApplication>
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/serial_backend.h"
#include "src/platform/desktop/common/serial/direct_serial_backend.h"
#include "src/platform/desktop/common/serial/j2534_driver_selection.h"
#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

// Exercises the SerialBackend contract against the direct implementation
// purely through the base-class pointer: get/set roundtrips hit the same
// storage the backend's own I/O logic reads, and closed-port I/O calls
// return their documented empty/error values without hardware.
TEST(TestDirectBackend, getSet_roundtrip_throughInterface)
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    b->SetAddSsmHeader(true);
    ASSERT_EQ(b->GetAddSsmHeader(), true);
    ASSERT_EQ(direct.add_ssm_header, true); // same storage the I/O paths read

    b->SetSerialPortBaudrate("10400");
    ASSERT_EQ(b->GetSerialPortBaudrate(), QString("10400"));
    ASSERT_EQ(direct.serial_port_baudrate, QString("10400"));

    b->SetKlineStartbyte(0x80);
    ASSERT_EQ(b->GetKlineStartbyte(), (uint8_t)0x80);

    b->SetCanSourceAddress(0x7E0);
    ASSERT_EQ(b->GetCanSourceAddress(), (uint32_t)0x7E0);

    b->SetSerialPortList(QStringList() << "ttyUSB0");
    ASSERT_EQ(b->GetSerialPortList(), QStringList() << "ttyUSB0");

    ASSERT_EQ(b->Qobject(), static_cast<QObject *>(&direct));
}

TEST(TestDirectBackend, closedPort_ioCalls_returnEmpty)
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    ASSERT_EQ(b->IsSerialPortOpen(), false);
    ASSERT_EQ(b->ReadSerialData(50), QByteArray());
    // Writes preserve the historical empty-array result on closed ports.
    ASSERT_EQ(b->WriteSerialData(QByteArray("\x01\x02", 2)), QByteArray());
    ASSERT_EQ(b->WriteSerialDataEchoCheck(QByteArray("\x01\x02", 2)), QByteArray());
    b->WaitForSource(); // default no-op must not block or crash
}

TEST(TestDirectBackend, j2534Selection_usesInstalledDllPathAfterVendorProbe)
{
    const QString vendor = "Tactrix Inc. - OpenPort 2.0 J2534 DLL";
    const QString dll_path = "C:\\Program Files (x86)\\OpenECU\\OpenPort 2.0\\op20pt32.dll";

    ASSERT_EQ(ResolveJ2534DllForConnection(vendor, dll_path, QStringList() << vendor), dll_path);
}

TEST(TestDirectBackend, j2534DriverViews_wow6432NodeVendorIsDiscoverable)
{
    QMap<QString, QString> native_view;
    native_view["Tactrix Inc. - OpenPort 2.0 J2534 DLL"] = "C:\\Program Files\\OpenECU\\OpenPort 2.0\\op20pt32.dll";

    QMap<QString, QString> wow64_view;
    wow64_view["Acme 32-bit-only J2534 DLL"] = "C:\\Program Files (x86)\\Acme\\acme_j2534.dll";

    QMap<QString, QString> merged = MergeJ2534DriverViews(wow64_view, native_view);

    ASSERT_EQ(merged.size(), 2);
    ASSERT_EQ(merged.value("Tactrix Inc. - OpenPort 2.0 J2534 DLL"),
              QString("C:\\Program Files\\OpenECU\\OpenPort 2.0\\op20pt32.dll"));
    ASSERT_EQ(merged.value("Acme 32-bit-only J2534 DLL"), QString("C:\\Program Files (x86)\\Acme\\acme_j2534.dll"));
}

TEST(TestDirectBackend, j2534DriverViews_laterViewOverwritesOnCollision)
{
    QMap<QString, QString> wow64_view;
    wow64_view["Shared Vendor"] = "C:\\wow64\\path.dll";

    QMap<QString, QString> native_view;
    native_view["Shared Vendor"] = "C:\\native\\path.dll";

    QMap<QString, QString> merged = MergeJ2534DriverViews(wow64_view, native_view);

    ASSERT_EQ(merged.size(), 1);
    ASSERT_EQ(merged.value("Shared Vendor"), QString("C:\\native\\path.dll"));
}

TEST(TestDirectBackend, makeDirectSerialBackend_buildsTheDirectBackend)
{
    const std::unique_ptr<SerialBackend> backend = MakeDirectSerialBackend();
    ASSERT_TRUE(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment(
        []
        {
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }));
} // namespace
