#include "apps/desktop/startup_vehicle_gate.h"

#include <cstdlib>
#include <optional>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

namespace
{

using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::config::testing::ConfigSessionFixture;
using fastecu::config::testing::Setting;
using ::testing::HasSubstr;

// For the paths that never save.
void UnexpectedSaveFailure(const Error& error)
{
    ADD_FAILURE() << "unexpected save failure: " << error.detail;
}

TEST(StartupVehicleGate, AnExistingSelectionDoesNotAsk)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("vehicle_id", "subaru-forester-v3"));
    ASSERT_TRUE(f.Initialize().has_value());
    int asked = 0;

    EXPECT_EQ(startup_vehicle_gate(
                  f.session,
                  [&asked]
                  {
                      ++asked;
                      return std::optional<std::size_t>{};
                  },
                  UnexpectedSaveFailure),
              std::nullopt);
    EXPECT_EQ(asked, 0);
}

TEST(StartupVehicleGate, AChosenVehicleIsSelectedSavedAndStartupContinues)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.Initialize().has_value());
    int reported = 0;

    EXPECT_EQ(startup_vehicle_gate(
                  f.session, [] { return std::optional<std::size_t>{1}; }, [&reported](const Error&) { ++reported; }),
              std::nullopt);

    EXPECT_EQ(reported, 0);
    ASSERT_NE(f.session.SelectedVehicle(), nullptr);
    EXPECT_EQ(f.session.SelectedVehicle()->id, "mitsubishi-colt-v2");
    // Saved: the session an in-app restart builds asks nothing.
    ASSERT_TRUE(f.Initialize().has_value());
    int asked = 0;
    EXPECT_EQ(startup_vehicle_gate(
                  f.session,
                  [&asked]
                  {
                      ++asked;
                      return std::optional<std::size_t>{};
                  },
                  UnexpectedSaveFailure),
              std::nullopt);
    EXPECT_EQ(asked, 0);
}

TEST(StartupVehicleGate, AFailedSaveIsReportedAndStartupContinues)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.Initialize().has_value());
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::kInvalidConfig, "cannot open file"};
    std::vector<Error> reported;

    EXPECT_EQ(startup_vehicle_gate(
                  f.session, [] { return std::optional<std::size_t>{1}; },
                  [&reported](const Error& error) { reported.push_back(error); }),
              std::nullopt);

    ASSERT_EQ(reported.size(), 1U);
    EXPECT_EQ(reported.front().kind, ErrorKind::kInvalidConfig);
    EXPECT_THAT(reported.front().detail, HasSubstr(f.paths.config_file));
    EXPECT_THAT(reported.front().detail, HasSubstr("cannot open file"));
    // The choice holds for this run...
    ASSERT_NE(f.session.SelectedVehicle(), nullptr);
    EXPECT_EQ(f.session.SelectedVehicle()->id, "mitsubishi-colt-v2");
    // ...and the next start asks again.
    ASSERT_TRUE(f.Initialize().has_value());
    EXPECT_EQ(f.session.SelectedVehicle(), nullptr);
}

TEST(StartupVehicleGate, ACancelEndsStartupWithExitCodeZero)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.Initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(
                  f.session, [] { return std::optional<std::size_t>{}; }, UnexpectedSaveFailure),
              std::optional<int>(EXIT_SUCCESS));
    EXPECT_EQ(f.session.SelectedVehicle(), nullptr);
}

TEST(StartupVehicleGate, AnInvalidRowIsTreatedAsACancel)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.Initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(
                  f.session, [] { return std::optional<std::size_t>{99}; }, UnexpectedSaveFailure),
              std::optional<int>(EXIT_SUCCESS));
}

} // namespace
