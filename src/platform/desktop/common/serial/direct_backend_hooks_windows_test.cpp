#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::j2534_tx_done;
    using SerialPortActionsDirect::resolve_port;
};

class TestDirectBackendHooksWindows : public ::testing::Test
{

  public:
    // Windows entries are J2534 vendor names: no split, every non-empty one is J2534.
};

TEST_F(TestDirectBackendHooksWindows, resolvePort_keepsTheVendorNameWhole)
{
    HookProbe probe;
    const auto resolved = probe.resolve_port("Tactrix Inc. - OpenPort 2.0 J2534 DLL");
    ASSERT_EQ(resolved.port, QString("Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
    ASSERT_TRUE(resolved.is_j2534);
}

TEST_F(TestDirectBackendHooksWindows, resolvePort_emptyEntryIsNotJ2534)
{
    HookProbe probe;
    ASSERT_TRUE(!probe.resolve_port("").is_j2534);
}

TEST_F(TestDirectBackendHooksWindows, txDone_isAlwaysTrue)
{
    HookProbe probe;
    ASSERT_TRUE(probe.j2534_tx_done());
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
