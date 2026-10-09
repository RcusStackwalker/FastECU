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
    auto snapshot = desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::kSsm, "SSM", policy());
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    EXPECT_EQ(snapshot->protocol, "SSM");
    EXPECT_EQ(snapshot->identities_by_id.at("rpm"), (logging::LoggerIdentity{"SSM", "rpm"}));
    values.set_selection({.protocol = "CDBG", .lower_panel_ids = {"rpm"}});
    EXPECT_EQ(snapshot->selection.lower_panel_ids, (std::vector<std::string>{"rpm", "coolant"}));
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
TEST(DesktopLoggingSnapshotAdapterTest, PreservesProtocolSelectionAndDisabledSsmOffsets)
{
    auto values = model({.parameters = {parameter("other", "OTHER"), parameter("off", "CAR_SSM", false),
                                        parameter("on", "CAR_SSM"), parameter("mut-off", "MUT_DMA", false),
                                        parameter("mut-on", "MUT_DMA"), parameter("cdbg-off", "CDBG", false)}},
                        {"other", "missing", "off", "on", "mut-off", "mut-on", "cdbg-off"});
    const auto ssm =
        desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::kSsm, "CAR_SSM", policy());
    const auto mut =
        desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::kMutDma, "ignored", policy());
    const auto cdbg =
        desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::kCdbg, "ignored", policy());
    ASSERT_THAT(ssm, fastecu::testing::IsOk());
    ASSERT_THAT(mut, fastecu::testing::IsOk());
    ASSERT_THAT(cdbg, fastecu::testing::IsOk());
    EXPECT_EQ(ssm->response_offsets, (std::vector<std::size_t>{2, 3}));
    EXPECT_FALSE(ssm->enabled_ids.contains("off"));
    EXPECT_TRUE(ssm->enabled_ids.contains("on"));
    EXPECT_EQ(ssm->session.channels().at(0).raw_assembly, logging::RawAssembly::kDecimalBytesConcatenated);
    EXPECT_EQ(mut->session.channels().size(), 1U);
    EXPECT_EQ(mut->session.channels().at(0).id, "mut-on");
    EXPECT_EQ(mut->session.channels().at(0).raw_assembly, logging::RawAssembly::kUnsignedIntegerDecimal);
    EXPECT_EQ(cdbg->session.channels().at(0).id, "cdbg-off");
    desktop::DesktopLoggerValues cache;
    cache.initialize(values);
    ASSERT_THAT(desktop::apply_log_sample(*ssm, {.channel_id = "off", .numeric_value = 9}, cache),
                fastecu::testing::IsOk());
    ASSERT_THAT(desktop::apply_log_sample(*ssm, {.channel_id = "on", .numeric_value = 2}, cache),
                fastecu::testing::IsOk());
    EXPECT_EQ(cache.parameter_value("CAR_SSM", "off"), "0.00");
    EXPECT_EQ(cache.parameter_value("CAR_SSM", "on"), "2.00");
}
TEST(DesktopLoggingValueAdapterTest, UnknownSamplesAndMissingCacheEntriesDoNotUpdateOtherIdentities)
{
    const auto values = model({.parameters = {parameter("rpm")}}, {"rpm"});
    const auto snapshot =
        desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::kSsm, "SSM", policy());
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    desktop::DesktopLoggerValues cache;
    cache.initialize(values);
    EXPECT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "unknown", .numeric_value = 8}, cache),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_EQ(cache.parameter_value("SSM", "rpm"), "0.00");
    desktop::DesktopLoggerValues empty;
    EXPECT_THAT(desktop::apply_log_sample(*snapshot, {.channel_id = "rpm", .numeric_value = 8}, empty),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
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
TEST(DesktopLoggingSnapshotAdapterTest, FirstConversionAndTargetAreCapturedByValue)
{
    auto p = parameter("rpm");
    p.conversions.push_back({"wrong", "x*2", "0.000", "0", "100", "1"});
    auto values = model({.parameters = {p}}, {"rpm"});
    auto snapshot = desktop::make_desktop_logging_snapshot(values, logging::LoggingProtocolId::kSsm, "SSM", policy());
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    EXPECT_EQ(snapshot->session.channels().at(0).decimal_precision, 2);
    EXPECT_EQ(snapshot->session.channels().at(0).from_byte_expression, "x");
    EXPECT_EQ(snapshot->session.channels().at(0).unit, "rpm");
    snapshot->target_is_ecu = false;
    const auto copy = *snapshot;
    snapshot->target_is_ecu = true;
    values.set_parameter_supported("SSM", "rpm", false);
    EXPECT_FALSE(copy.target_is_ecu);
    EXPECT_TRUE(copy.enabled_ids.contains("rpm"));
}
TEST(DesktopLoggingSnapshotAdapterTest, RejectsMalformedChannelsAndDuplicateIds)
{
    for (int problem = 0; problem < 10; ++problem)
    {
        auto p = parameter("rpm");
        std::vector<std::string> ids{"rpm"};
        auto protocol = logging::LoggingProtocolId::kSsm;
        QString filter = "SSM";
        logging::LoggerDefinition def;
        switch (problem)
        {
        case 0:
            p.conversions.clear();
            break;
        case 1:
            p.conversions.front().expr.clear();
            break;
        case 2:
            p.address = "invalid";
            break;
        case 3:
            p.length = "invalid";
            break;
        case 4:
            p.length = "0";
            break;
        case 5:
            p.conversions.front().format = "0." + std::string(256, '0');
            break;
        case 6:
            def.parameters.push_back(p);
            break;
        case 7:
            ids.push_back("rpm");
            break;
        case 8:
            filter.clear();
            break;
        case 9:
            protocol = static_cast<logging::LoggingProtocolId>(999);
            break;
        default:
            FAIL() << "invalid malformed-channel test case";
            return;
        }
        def.parameters.push_back(p);
        const auto values = model(std::move(def), std::move(ids));
        EXPECT_THAT(desktop::make_desktop_logging_snapshot(values, protocol, filter, policy()),
                    fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig))
            << problem;
    }
}
} // namespace
