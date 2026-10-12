#include "src/backend/logging/logging_sample_resolution.h"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "src/backend/logging/logger_model.h"
#include "src/backend/logging/logging_run_snapshot.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::logging
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsOk;

fastecu::Result<LoggingRunSnapshot> Snapshot(LoggingProtocolId protocol, std::string key, bool supported)
{
    LoggerModel model;
    model.InstallDefinition({.parameters = {{.protocol = key,
                                             .id = "rpm",
                                             .address = "10",
                                             .length = "1",
                                             .enabled = supported,
                                             .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}}}});
    model.SetSelection({.protocol = key, .lower_panel_ids = {"rpm"}});
    return PrepareLoggingRun(model, protocol, key,
                             {.poll_timeout = 100ms,
                              .car_silence_miss_threshold = 3,
                              .reconnect_attempt_threshold = 5,
                              .reconnect_retry_period = 10},
                             LoggingTarget::kEcu);
}

TEST(LoggingSampleResolution, DerivesIdentityAndPrecisionFromCapturedRun)
{
    const auto run = Snapshot(LoggingProtocolId::kSsm, "SSM", true);
    ASSERT_THAT(run, IsOk());
    const auto result = ResolveLogSample(*run, {.channel_id = "parameter:rpm", .numeric_value = 1234.5});
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->identity, (LoggerIdentity{"SSM", "rpm"}));
    EXPECT_EQ(result->numeric_value, 1234.5);
    EXPECT_EQ(result->decimal_precision, 2);
}

TEST(LoggingSampleResolution, ExplicitUnknownSelectionResolvesIndependentlyOfDefaultEligibility)
{
    const auto ssm = Snapshot(LoggingProtocolId::kSsm, "SSM", false);
    const auto cdbg = Snapshot(LoggingProtocolId::kCdbg, "CDBG", false);
    ASSERT_THAT(ssm, IsOk());
    ASSERT_THAT(cdbg, IsOk());
    const auto accepted = ResolveLogSample(*ssm, {.channel_id = "parameter:rpm", .numeric_value = 9});
    ASSERT_THAT(accepted, IsOk());
    EXPECT_EQ(accepted->kind, LoggingMeasurementKind::kParameter);
    const auto shown = ResolveLogSample(*cdbg, {.channel_id = "parameter:rpm", .numeric_value = 9});
    ASSERT_THAT(shown, IsOk());
    EXPECT_EQ(shown->identity, (LoggerIdentity{"CDBG", "rpm"}));
}

TEST(LoggingSampleResolution, UnknownIdIsAnErrorRatherThanADisabledSkip)
{
    const auto run = Snapshot(LoggingProtocolId::kSsm, "SSM", true);
    ASSERT_THAT(run, IsOk());
    EXPECT_THAT(ResolveLogSample(*run, {.channel_id = "unknown", .numeric_value = 9}),
                fastecu::testing::IsErr(ErrorKind::kInternal));
}
} // namespace
} // namespace fastecu::logging
