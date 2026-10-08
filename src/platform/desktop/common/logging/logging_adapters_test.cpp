#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <chrono>
#include <gtest/gtest.h>

namespace desktop = fastecu::desktop::logging;
namespace logging = fastecu::logging;
using namespace std::chrono_literals;

namespace
{
logging::LoggingPolicy policy()
{
    return {.poll_timeout = 100ms,
            .car_silence_miss_threshold = 3,
            .reconnect_attempt_threshold = 5,
            .reconnect_retry_period = 10};
}
logging::LoggerParameter parameter(std::string id, std::string protocol = "SSM", bool enabled = true)
{
    return {.protocol = std::move(protocol),
            .id = std::move(id),
            .address = "000010",
            .length = "1",
            .enabled = enabled,
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}
logging::LoggerModel model(logging::LoggerDefinition def, std::vector<std::string> ids)
{
    logging::LoggerModel result;
    result.install_definition(std::move(def));
    result.set_selection({.protocol = "SSM", .lower_panel_ids = std::move(ids)});
    return result;
}
TEST(DesktopLoggingSnapshotAdapterTest, StableIdentitySurvivesSelectionEditsAndDefinitionReordering)
{
    auto values =
        model({.parameters = {parameter("coolant"), parameter("rpm"), parameter("rpm", "CDBG")}}, {"rpm", "coolant"});
    auto snapshot = desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::Ssm, "SSM", policy(),
                                                           logging::LoggingTarget::Ecu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    EXPECT_EQ(snapshot->protocol_key(), "SSM");
    values.set_selection({.protocol = "CDBG", .lower_panel_ids = {"rpm"}});
    EXPECT_EQ(snapshot->selection().lower_panel_ids, (std::vector<std::string>{"rpm", "coolant"}));
    auto reordered =
        model({.parameters = {parameter("rpm", "CDBG"), parameter("rpm"), parameter("coolant")}}, {"coolant"});
    desktop::DesktopLoggerValues cache;
    cache.initialize(reordered);
    ASSERT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "rpm", .numeric_value = 1234.5}, cache),
                fastecu::testing::IsOk());
    EXPECT_EQ(cache.parameter_value("SSM", "rpm"), "1234.50");
    EXPECT_EQ(cache.parameter_value("CDBG", "rpm"), "0.00");
    EXPECT_EQ(cache.parameter_value("SSM", "coolant"), "0.00");
}
TEST(DesktopLoggingValueAdapterTest, UnknownSamplesAndMissingCacheEntriesDoNotUpdateOtherIdentities)
{
    const auto values = model({.parameters = {parameter("rpm")}}, {"rpm"});
    const auto snapshot = desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::Ssm, "SSM",
                                                                 policy(), logging::LoggingTarget::Ecu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    desktop::DesktopLoggerValues cache;
    cache.initialize(values);
    EXPECT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "unknown", .numeric_value = 8}, cache),
                fastecu::testing::IsErr(fastecu::ErrorKind::Internal));
    EXPECT_EQ(cache.parameter_value("SSM", "rpm"), "0.00");
    desktop::DesktopLoggerValues empty;
    EXPECT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "rpm", .numeric_value = 8}, empty),
                fastecu::testing::IsErr(fastecu::ErrorKind::Internal));
}
TEST(DesktopLoggingValueAdapterTest, CacheNamespacesAndFixedDecimalFormatting)
{
    const auto values =
        model({.parameters = {parameter("rpm")}, .switches = {{.protocol = "SSM", .id = "rpm"}}}, {"rpm"});
    desktop::DesktopLoggerValues cache;
    cache.initialize(values);
    EXPECT_EQ(cache.parameter_value("SSM", "rpm"), "0.00");
    EXPECT_EQ(cache.switch_value("SSM", "rpm"), "0");
    EXPECT_EQ(desktop::format_logging_value(2.0, 0), "2");
    EXPECT_EQ(desktop::format_logging_value(-1.25, 2), "-1.25");
    EXPECT_EQ(desktop::format_logging_value(1.2, 3), "1.200");
}
TEST(DesktopLoggingSnapshotAdapter, EmptyUnitsAndDisabledSamplesReachDisplayAdapter)
{
    auto p = parameter("rpm");
    p.conversions.front().units.clear();
    auto values = model({.parameters = {p, parameter("off", "SSM", false)}}, {"rpm", "off"});
    const auto snapshot = desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::Ssm, "SSM",
                                                                 policy(), logging::LoggingTarget::Tcu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    EXPECT_EQ(snapshot->target(), logging::LoggingTarget::Tcu);
    desktop::DesktopLoggerValues cache;
    cache.initialize(values);
    ASSERT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "rpm", .numeric_value = 2}, cache),
                fastecu::testing::IsOk());
    ASSERT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "off", .numeric_value = 9}, cache),
                fastecu::testing::IsOk());
    EXPECT_EQ(cache.parameter_value("SSM", "rpm"), "2.00");
    EXPECT_EQ(cache.parameter_value("SSM", "off"), "0.00");
}
} // namespace
