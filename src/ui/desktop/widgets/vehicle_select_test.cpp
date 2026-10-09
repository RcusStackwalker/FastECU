#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/ui/desktop/widgets/vehicle_select.h"

using fastecu::config::testing::ConfigSessionFixture;

TEST(VehicleSelectTest, choosingRecordsTheRowWithoutTouchingTheSession)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    ASSERT_TRUE(f.session.select_row(2).has_value());
    const auto before = f.session.settings();

    VehicleSelect dialog{f.session}; // opens on the session's row (Subaru Forester)
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

    ASSERT_EQ(dialog.result(), int(QDialog::Accepted));
    ASSERT_EQ(dialog.chosen_row(), std::optional<std::size_t>(2));
    ASSERT_TRUE(f.session.settings() == before);
}

TEST(VehicleSelectTest, rejectingLeavesNoChoice)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    VehicleSelect dialog{f.session};
    dialog.reject();
    ASSERT_TRUE(!dialog.chosen_row().has_value());
}

// The startup vehicle gate opens the dialog on a session with no vehicle.
TEST(VehicleSelectTest, withNoSelectionItOpensOnTheFirstMakeModelAndVersion)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    ASSERT_TRUE(f.session.selected_vehicle() == nullptr);

    VehicleSelect dialog{f.session}; // Mitsubishi sorts first; its only vehicle is the Colt
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

    ASSERT_EQ(dialog.chosen_row(), std::optional<std::size_t>(1));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
