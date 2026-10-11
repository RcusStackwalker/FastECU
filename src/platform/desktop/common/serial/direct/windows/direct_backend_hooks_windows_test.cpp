#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::J2534TxDone;
    using SerialPortActionsDirect::ResolvePort;
};

// Windows entries are J2534 vendor names: no split, every non-empty one is J2534.
TEST(TestDirectBackendHooksWindows, resolvePort_keepsTheVendorNameWhole)
{
    HookProbe probe;
    const auto resolved = probe.ResolvePort("Tactrix Inc. - OpenPort 2.0 J2534 DLL");
    ASSERT_EQ(resolved.port, QString("Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
    ASSERT_TRUE(resolved.is_j2534);
}

TEST(TestDirectBackendHooksWindows, resolvePort_emptyEntryIsNotJ2534)
{
    HookProbe probe;
    ASSERT_TRUE(!probe.ResolvePort("").is_j2534);
}

TEST(TestDirectBackendHooksWindows, txDone_isAlwaysTrue)
{
    HookProbe probe;
    ASSERT_TRUE(probe.J2534TxDone());
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
