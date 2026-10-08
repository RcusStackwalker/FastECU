#include "src/backend/logging/logging_csv_columns.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <chrono>
#include <gtest/gtest.h>

namespace fastecu::logging
{
TEST(LoggingCsvColumnsTest, UsesCapturedIdentityAndOrderAcrossRepeatedDisplayPositions)
{
    const auto run = []
    {
        LoggerModel model;
        model.install_definition(
            {.parameters = {{.protocol = "SSM",
                             .id = "same",
                             .name = "Engine",
                             .address = "10",
                             .length = "1",
                             .conversions = {{.units = "rpm", .expr = "x", .format = "0"}}}},
             .switches = {{.protocol = "SSM", .id = "same", .name = "Flag", .address = "20", .sample_bit = "5"}}});
        model.set_selection(
            {.protocol = "SSM", .gauge_ids = {"same"}, .lower_panel_ids = {"same", "same"}, .switch_ids = {"same"}});
        auto snapshot = prepare_logging_run(model, LoggingProtocolId::Ssm, "SSM",
                                            {.poll_timeout = std::chrono::milliseconds{50},
                                             .car_silence_miss_threshold = 2,
                                             .reconnect_attempt_threshold = 3,
                                             .reconnect_retry_period = 0},
                                            LoggingTarget::Ecu);
        model.set_selection({.protocol = "OTHER"});
        return snapshot;
    }();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    const auto columns = prepare_logging_csv_columns(*run);
    ASSERT_THAT(columns, fastecu::testing::IsOk());
    ASSERT_EQ(columns->size(), 4U);
    for (std::size_t i : {0U, 1U, 2U})
    {
        EXPECT_EQ(columns->at(i).kind, LoggingMeasurementKind::Parameter);
        EXPECT_EQ(columns->at(i).identity, (LoggerIdentity{"SSM", "same"}));
        EXPECT_EQ(columns->at(i).name, "Engine");
    }
    EXPECT_EQ(columns->at(3).kind, LoggingMeasurementKind::Switch);
    EXPECT_EQ(columns->at(3).identity, (LoggerIdentity{"SSM", "same"}));
    EXPECT_EQ(columns->at(3).name, "Flag");
}
} // namespace fastecu::logging
