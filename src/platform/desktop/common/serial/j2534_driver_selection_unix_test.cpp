#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// Unix check_serial_ports() entries are "<portName> - <description>"; only the
// description tells an adapter from a Bluetooth or debug-console port (#243).
class TestJ2534DriverSelectionUnix : public ::testing::Test
{

  public:
};

TEST_F(TestJ2534DriverSelectionUnix, capableEntry_matchesOnlyTheAdapterDescription)
{
    ASSERT_TRUE(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
    ASSERT_TRUE(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0")); // case-insensitive
    // macOS enumerates these ahead of the adapter; driving ISO-15765 over one
    // yields a timeout per exchange, never a response.
    ASSERT_TRUE(!isJ2534CapableEntry(u"cu.Bluetooth-Incoming-Port - "));
    ASSERT_TRUE(!isJ2534CapableEntry(u"cu.debug-console - "));
    ASSERT_TRUE(!isJ2534CapableEntry(u"ttyUSB0 - USB Serial"));
    ASSERT_TRUE(!isJ2534CapableEntry(u"ttyUSB0")); // no separator at all
    ASSERT_TRUE(!isJ2534CapableEntry(u""));
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
