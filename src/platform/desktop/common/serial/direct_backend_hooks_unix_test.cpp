#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::append_j2534_interfaces;
    using SerialPortActionsDirect::resolve_port;
};

class TestDirectBackendHooksUnix : public ::testing::Test
{

  public:
    // macOS lists this port ahead of the adapter; it must fall back to plain serial.
};

TEST_F(TestDirectBackendHooksUnix, resolvePort_prefixesAndSplitsAnAdapterEntry)
{
    HookProbe probe;
    const auto resolved = probe.resolve_port("cu.usbmodem0 - OpenPort 2.0");
    ASSERT_EQ(resolved.port, QString("/dev/cu.usbmodem0"));
    ASSERT_TRUE(resolved.is_j2534);
}

TEST_F(TestDirectBackendHooksUnix, resolvePort_plainSerialEntryIsNotJ2534)
{
    HookProbe probe;
    const auto bluetooth = probe.resolve_port("cu.Bluetooth-Incoming-Port - ");
    ASSERT_EQ(bluetooth.port, QString("/dev/cu.Bluetooth-Incoming-Port"));
    ASSERT_TRUE(!bluetooth.is_j2534);
    const auto usb = probe.resolve_port("ttyUSB0 - USB Serial");
    ASSERT_EQ(usb.port, QString("/dev/ttyUSB0"));
    ASSERT_TRUE(!usb.is_j2534);
}

TEST_F(TestDirectBackendHooksUnix, appendJ2534Interfaces_leavesTheListUntouched)
{
    HookProbe probe;
    QStringList ports{"cu.usbmodem0 - OpenPort 2.0"};
    probe.append_j2534_interfaces(ports);
    ASSERT_EQ(ports, QStringList{"cu.usbmodem0 - OpenPort 2.0"});
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
