#include "apps/desktop/startup_vehicle_gate.h"

#include <cstdlib>
#include <optional>

#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

namespace
{

using fastecu::config::testing::ConfigSessionFixture;
using fastecu::config::testing::setting;

TEST(StartupVehicleGate, AnExistingSelectionDoesNotAsk)
{
    ConfigSessionFixture f;
    f.put_settings(setting("vehicle_id", "subaru-forester-v3"));
    ASSERT_TRUE(f.initialize().has_value());
    int asked = 0;

    EXPECT_EQ(startup_vehicle_gate(f.session,
                                   [&asked]
                                   {
                                       ++asked;
                                       return std::optional<std::size_t>{};
                                   }),
              std::nullopt);
    EXPECT_EQ(asked, 0);
}

TEST(StartupVehicleGate, AChosenVehicleIsSelectedSavedAndStartupContinues)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(f.session, [] { return std::optional<std::size_t>{1}; }), std::nullopt);

    ASSERT_NE(f.session.selected_vehicle(), nullptr);
    EXPECT_EQ(f.session.selected_vehicle()->id, "mitsubishi-colt-v2");
    // Saved: the session an in-app restart builds asks nothing.
    ASSERT_TRUE(f.initialize().has_value());
    int asked = 0;
    EXPECT_EQ(startup_vehicle_gate(f.session,
                                   [&asked]
                                   {
                                       ++asked;
                                       return std::optional<std::size_t>{};
                                   }),
              std::nullopt);
    EXPECT_EQ(asked, 0);
}

TEST(StartupVehicleGate, ACancelEndsStartupWithExitCodeZero)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(f.session, [] { return std::optional<std::size_t>{}; }),
              std::optional<int>(EXIT_SUCCESS));
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
}

TEST(StartupVehicleGate, AnInvalidRowIsTreatedAsACancel)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(f.session, [] { return std::optional<std::size_t>{99}; }),
              std::optional<int>(EXIT_SUCCESS));
}

} // namespace
