#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::AppendJ2534Interfaces;
    using SerialPortActionsDirect::ResolvePort;
};

TEST(TestDirectBackendHooksUnix, resolvePort_prefixesAndSplitsAnAdapterEntry)
{
    HookProbe probe;
    const auto resolved = probe.ResolvePort("cu.usbmodem0 - OpenPort 2.0");
    ASSERT_EQ(resolved.port, QString("/dev/cu.usbmodem0"));
    ASSERT_TRUE(resolved.is_j2534);
}

// macOS lists this port ahead of the adapter; it must fall back to plain serial.
TEST(TestDirectBackendHooksUnix, resolvePort_plainSerialEntryIsNotJ2534)
{
    HookProbe probe;
    const auto bluetooth = probe.ResolvePort("cu.Bluetooth-Incoming-Port - ");
    ASSERT_EQ(bluetooth.port, QString("/dev/cu.Bluetooth-Incoming-Port"));
    ASSERT_TRUE(!bluetooth.is_j2534);
    const auto usb = probe.ResolvePort("ttyUSB0 - USB Serial");
    ASSERT_EQ(usb.port, QString("/dev/ttyUSB0"));
    ASSERT_TRUE(!usb.is_j2534);
}

TEST(TestDirectBackendHooksUnix, appendJ2534Interfaces_leavesTheListUntouched)
{
    HookProbe probe;
    QStringList ports{"cu.usbmodem0 - OpenPort 2.0"};
    probe.AppendJ2534Interfaces(ports);
    ASSERT_EQ(ports, QStringList{"cu.usbmodem0 - OpenPort 2.0"});
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
