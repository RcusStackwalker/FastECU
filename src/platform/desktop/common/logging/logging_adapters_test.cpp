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
logging::LoggingPolicy Policy()
{
    return {.poll_timeout = 100ms,
            .car_silence_miss_threshold = 3,
            .reconnect_attempt_threshold = 5,
            .reconnect_retry_period = 10};
}
logging::LoggerParameter BuildParameter(std::string id, std::string protocol = "SSM", bool enabled = true)
{
    return {.protocol = std::move(protocol),
            .id = std::move(id),
            .address = "000010",
            .length = "1",
            .enabled = enabled,
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}
logging::LoggerModel BuildModel(logging::LoggerDefinition def, std::vector<std::string> ids)
{
    logging::LoggerModel result;
    result.InstallDefinition(std::move(def));
    result.SetSelection({.protocol = "SSM", .lower_panel_ids = std::move(ids)});
    return result;
}
TEST(DesktopLoggingSnapshotAdapterTest, StableIdentitySurvivesSelectionEditsAndDefinitionReordering)
{
    auto values =
        BuildModel({.parameters = {BuildParameter("coolant"), BuildParameter("rpm"), BuildParameter("rpm", "CDBG")}},
                   {"rpm", "coolant"});
    auto snapshot = desktop::MakeDesktopLoggingSnapshot(values, logging::LoggingProtocolId::kSsm, "SSM", Policy(),
                                                        logging::LoggingTarget::kEcu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    EXPECT_EQ(snapshot->ProtocolKey(), "SSM");
    values.SetSelection({.protocol = "CDBG", .lower_panel_ids = {"rpm"}});
    EXPECT_EQ(snapshot->Selection().lower_panel_ids, (std::vector<std::string>{"rpm", "coolant"}));
    auto reordered = BuildModel(
        {.parameters = {BuildParameter("rpm", "CDBG"), BuildParameter("rpm"), BuildParameter("coolant")}}, {"coolant"});
    desktop::DesktopLoggerValues cache;
    cache.Initialize(reordered);
    ASSERT_THAT(desktop::ApplyLogSample(*snapshot, {.channel_id = "rpm", .numeric_value = 1234.5}, cache),
                fastecu::testing::IsOk());
    EXPECT_EQ(cache.ParameterValue("SSM", "rpm"), "1234.50");
    EXPECT_EQ(cache.ParameterValue("CDBG", "rpm"), "0.00");
    EXPECT_EQ(cache.ParameterValue("SSM", "coolant"), "0.00");
}
TEST(DesktopLoggingValueAdapterTest, UnknownSamplesAndMissingCacheEntriesDoNotUpdateOtherIdentities)
{
    const auto values = BuildModel({.parameters = {BuildParameter("rpm")}}, {"rpm"});
    const auto snapshot = desktop::MakeDesktopLoggingSnapshot(values, logging::LoggingProtocolId::kSsm, "SSM", Policy(),
                                                              logging::LoggingTarget::kEcu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    desktop::DesktopLoggerValues cache;
    cache.Initialize(values);
    EXPECT_THAT(
        desktop::ApplyLogSample(*snapshot, {.channel_id = "unknown", .numeric_value = 8}, cache),
        fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal,
                                    ::testing::AllOf(::testing::HasSubstr("SSM"), ::testing::HasSubstr("unknown"))));
    EXPECT_EQ(cache.ParameterValue("SSM", "rpm"), "0.00");
    desktop::DesktopLoggerValues empty;
    EXPECT_THAT(desktop::ApplyLogSample(*snapshot, {.channel_id = "rpm", .numeric_value = 8}, empty),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
}
TEST(DesktopLoggingValueAdapterTest, CacheNamespacesAndFixedDecimalFormatting)
{
    const auto values =
        BuildModel({.parameters = {BuildParameter("rpm")}, .switches = {{.protocol = "SSM", .id = "rpm"}}}, {"rpm"});
    desktop::DesktopLoggerValues cache;
    cache.Initialize(values);
    EXPECT_EQ(cache.ParameterValue("SSM", "rpm"), "0.00");
    EXPECT_EQ(cache.SwitchValue("SSM", "rpm"), "0");
    EXPECT_EQ(desktop::FormatLoggingValue(2.0, 0), "2");
    EXPECT_EQ(desktop::FormatLoggingValue(-1.25, 2), "-1.25");
    EXPECT_EQ(desktop::FormatLoggingValue(1.2, 3), "1.200");
}
TEST(DesktopLoggingSnapshotAdapter, EmptyUnitsAndDisabledSamplesReachDisplayAdapter)
{
    auto p = BuildParameter("rpm");
    p.conversions.front().units.clear();
    auto values = BuildModel({.parameters = {p, BuildParameter("off", "SSM", false)}}, {"rpm", "off"});
    const auto snapshot = desktop::MakeDesktopLoggingSnapshot(values, logging::LoggingProtocolId::kSsm, "SSM", Policy(),
                                                              logging::LoggingTarget::kTcu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    EXPECT_EQ(snapshot->Target(), logging::LoggingTarget::kTcu);
    desktop::DesktopLoggerValues cache;
    cache.Initialize(values);
    ASSERT_THAT(desktop::ApplyLogSample(*snapshot, {.channel_id = "rpm", .numeric_value = 2}, cache),
                fastecu::testing::IsOk());
    ASSERT_THAT(desktop::ApplyLogSample(*snapshot, {.channel_id = "off", .numeric_value = 9}, cache),
                fastecu::testing::IsOk());
    EXPECT_EQ(cache.ParameterValue("SSM", "rpm"), "2.00");
    EXPECT_EQ(cache.ParameterValue("SSM", "off"), "0.00");
}
} // namespace
