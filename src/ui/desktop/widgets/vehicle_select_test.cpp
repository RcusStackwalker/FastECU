#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/widgets/vehicle_select.h"

using fastecu::config::testing::ConfigSessionFixture;
using fastecu::testing::IsOk;

TEST(VehicleSelectTest, choosingRecordsTheRowWithoutTouchingTheSession)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    ASSERT_THAT(f.session.SelectRow(2), IsOk());
    const auto before = f.session.Settings();

    VehicleSelect dialog{f.session}; // opens on the session's row (Subaru Forester)
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "carModelSelected", Qt::DirectConnection));

    ASSERT_EQ(dialog.result(), int(QDialog::Accepted));
    ASSERT_EQ(dialog.chosenRow(), std::optional<std::size_t>(2));
    ASSERT_TRUE(f.session.Settings() == before);
}

TEST(VehicleSelectTest, rejectingLeavesNoChoice)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    VehicleSelect dialog{f.session};
    dialog.reject();
    ASSERT_TRUE(!dialog.chosenRow().has_value());
}

// The startup vehicle gate opens the dialog on a session with no vehicle.
TEST(VehicleSelectTest, withNoSelectionItOpensOnTheFirstMakeModelAndVersion)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    ASSERT_TRUE(f.session.SelectedVehicle() == nullptr);

    VehicleSelect dialog{f.session}; // Mitsubishi sorts first; its only vehicle is the Colt
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "carModelSelected", Qt::DirectConnection));

    ASSERT_EQ(dialog.chosenRow(), std::optional<std::size_t>(1));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
