#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// Windows entries come from getAllJ2534DriversNames(), carry no description,
// and open_serial_port() drives every one of them through J2534 -- rejecting
// them here would regress Windows.
class TestJ2534DriverSelectionWindows : public ::testing::Test
{

  public:
};

TEST_F(TestJ2534DriverSelectionWindows, capableEntry_acceptsEveryNonEmptyEntry)
{
    ASSERT_TRUE(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
    ASSERT_TRUE(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0"));
    ASSERT_TRUE(isJ2534CapableEntry(u"Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
    ASSERT_TRUE(isJ2534CapableEntry(u"Acme J2534 DLL"));
    ASSERT_TRUE(!isJ2534CapableEntry(u""));
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
