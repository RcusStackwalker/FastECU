#include "src/backend/logging/logging_run_snapshot.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/logging/logger_model.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::logging
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

LoggingPolicy Policy()
{
    return {.poll_timeout = 100ms,
            .car_silence_miss_threshold = 3,
            .reconnect_attempt_threshold = 5,
            .reconnect_retry_period = 10};
}
LoggerParameter Parameter(std::string id, std::string protocol = "SSM", bool enabled = true)
{
    return {.protocol = std::move(protocol),
            .id = std::move(id),
            .address = "000010",
            .length = "1",
            .enabled = enabled,
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}
LoggerModel Model(LoggerDefinition definition, std::vector<std::string> ids)
{
    LoggerModel result;
    result.InstallDefinition(std::move(definition));
    result.SetSelection({.protocol = "SSM", .lower_panel_ids = std::move(ids)});
    return result;
}

TEST(LoggingRunSnapshot, OwnsMeasurementPresentationAfterModelDestruction)
{
    const auto run = []
    {
        auto rpm = Parameter("rpm");
        rpm.name = "Engine RPM";
        const auto source = Model({.parameters = {rpm}}, {"rpm"});
        return PrepareLoggingRun(source, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu);
    }();
    ASSERT_THAT(run, IsOk());
    const auto *measurement = run->FindMeasurement(LoggingMeasurementKind::kParameter, "rpm");
    ASSERT_NE(measurement, nullptr);
    EXPECT_EQ(measurement->identity, (LoggerIdentity{"SSM", "rpm"}));
    EXPECT_EQ(measurement->name, "Engine RPM");
    EXPECT_EQ(measurement->unit, "rpm");
    EXPECT_EQ(measurement->decimal_precision, 2);
    EXPECT_EQ(measurement->support, EcuSupport::kUnknown);
}

TEST(LoggingRunSnapshot, AcquiresSelectedUnionAndSharesRepeatedParameterIdentity)
{
    auto values = Model({.parameters = {Parameter("rpm")},
                         .switches = {{.protocol = "SSM", .id = "rpm", .address = "20", .sample_bit = "7"}}},
                        {});
    values.SetSelection(
        {.protocol = "SSM", .gauge_ids = {"rpm"}, .lower_panel_ids = {"rpm", "rpm"}, .switch_ids = {"rpm"}});
    const auto run = PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu);
    ASSERT_THAT(run, IsOk());
    ASSERT_EQ(run->Session().Channels().size(), 2U);
    ASSERT_NE(run->FindMeasurement(LoggingMeasurementKind::kParameter, "rpm"), nullptr);
    ASSERT_NE(run->FindMeasurement(LoggingMeasurementKind::kSwitch, "rpm"), nullptr);
    EXPECT_EQ(run->Session().Channels()[0].id, "parameter:rpm");
    EXPECT_EQ(run->Session().Channels()[1].id, "switch:rpm");
}
TEST(LoggingRunSnapshot, RejectsKnownUnsupportedSelectedMeasurement)
{
    auto values = Model({.parameters = {Parameter("rpm")}}, {"rpm"});
    values.SetParameterSupport("SSM", "rpm", EcuSupport::kUnsupported);
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu),
                IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("unsupported"))));
}
TEST(LoggingRunSnapshot, ExplicitUnknownMutSelectionIsValidatedEvenWhenXmlDisabled)
{
    const auto values = Model({.parameters = {Parameter("rpm", "MUT_DMA", false)}}, {"rpm"});
    const auto run = PrepareLoggingRun(values, LoggingProtocolId::kMutDma, "ignored", Policy(), LoggingTarget::kEcu);
    ASSERT_THAT(run, IsOk());
    ASSERT_EQ(run->Session().Channels().size(), 1U);
}

TEST(LoggingRunSnapshot, AcquiresGaugeOnlyAndSwitchOnlySelections)
{
    auto values = Model({.parameters = {Parameter("rpm")},
                         .switches = {{.protocol = "SSM", .id = "flag", .address = "20", .sample_bit = "5"}}},
                        {});
    values.SetSelection({.protocol = "SSM", .gauge_ids = {"rpm"}});
    const auto gauge = PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kTcu);
    ASSERT_THAT(gauge, IsOk());
    ASSERT_EQ(gauge->Session().Channels().size(), 1U);
    EXPECT_EQ(gauge->Target(), LoggingTarget::kTcu);
    values.SetSelection({.protocol = "SSM", .switch_ids = {"flag"}});
    const auto flag = PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu);
    ASSERT_THAT(flag, IsOk());
    ASSERT_EQ(flag->Session().Channels().size(), 1U);
    EXPECT_EQ(flag->Session().Channels()[0].sample_bit, 5);
}

TEST(LoggingRunSnapshot, ValidatesOnlyParticipatingChannels)
{
    for (const auto& [protocol, key] :
         std::vector<std::pair<LoggingProtocolId, std::string>>{{LoggingProtocolId::kSsm, "SSM"},
                                                                {LoggingProtocolId::kMutDma, "MUT_DMA"},
                                                                {LoggingProtocolId::kCdbg, "CDBG"}})
    {
        SCOPED_TRACE(key);
        auto invalid = Parameter("off", key, false);
        invalid.conversions.front().format = "banana";
        const auto values = Model({.parameters = {invalid, Parameter("on", key)}}, {"off", "on"});
        const auto result = PrepareLoggingRun(values, protocol, key, Policy(), LoggingTarget::kEcu);
        EXPECT_THAT(result,
                    IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr(key), HasSubstr("off"), HasSubstr("format"))));
    }
}

TEST(LoggingRunSnapshot, CapturesOwnedInputs)
{
    auto snapshot = []
    {
        auto source = Model({.parameters = {Parameter("rpm"), Parameter("rpm", "CDBG"), Parameter("coolant")}},
                            {"rpm", "coolant"});
        auto result = PrepareLoggingRun(source, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kTcu);
        source.SetSelection({.protocol = "CDBG", .lower_panel_ids = {"rpm"}});
        source.SetParameterSupport("SSM", "rpm", fastecu::logging::EcuSupport::kUnsupported);
        return result;
    }();
    ASSERT_THAT(snapshot, IsOk());
    EXPECT_EQ(snapshot->ProtocolKey(), "SSM");
    EXPECT_EQ(snapshot->Target(), LoggingTarget::kTcu);
    ASSERT_NE(snapshot->FindMeasurement(LoggingMeasurementKind::kParameter, "rpm"), nullptr);
    EXPECT_THAT(snapshot->Selection().lower_panel_ids, ElementsAre("rpm", "coolant"));
    ASSERT_EQ(snapshot->Session().Channels().size(), 2U);
    EXPECT_EQ(snapshot->Session().Channels()[0].id, "parameter:rpm");
    const auto copy = *snapshot;
    EXPECT_EQ(copy.Target(), LoggingTarget::kTcu);
}

TEST(LoggingRunSnapshot, RejectsDuplicateParticipatingIds)
{
    const auto duplicate_definition = Model({.parameters = {Parameter("rpm"), Parameter("rpm")}}, {"rpm"});
    EXPECT_THAT(
        PrepareLoggingRun(duplicate_definition, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu),
        IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr("duplicate"))));
    const auto duplicate_selection = Model({.parameters = {Parameter("rpm")}}, {"rpm", "rpm"});
    const auto repeated =
        PrepareLoggingRun(duplicate_selection, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu);
    ASSERT_THAT(repeated, IsOk());
    EXPECT_EQ(repeated->Session().Channels().size(), 1U);
}

TEST(LoggingRunSnapshot, RejectsUnresolvedSelectionWithActionableIdentity)
{
    const auto values = Model({}, {"missing"});
    for (const auto protocol : {LoggingProtocolId::kSsm, LoggingProtocolId::kMutDma, LoggingProtocolId::kCdbg})
    {
        EXPECT_THAT(PrepareLoggingRun(values, protocol, "SSM", Policy(), LoggingTarget::kEcu),
                    IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("missing"), HasSubstr("unresolved"))));
    }
}

class LoggingRunSnapshotInvalidEnum : public ::testing::TestWithParam<int>
{
};

TEST_P(LoggingRunSnapshotInvalidEnum, RejectsUnknownProtocolAndTarget)
{
    const auto values = Model({.parameters = {Parameter("rpm")}}, {"rpm"});
    const auto input = GetParam();
    EXPECT_THAT(PrepareLoggingRun(values, static_cast<LoggingProtocolId>(input), "SSM", Policy(), LoggingTarget::kEcu),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", Policy(), static_cast<LoggingTarget>(input)),
                IsErr(ErrorKind::kInvalidConfig));
}

INSTANTIATE_TEST_SUITE_P(UnknownInputs, LoggingRunSnapshotInvalidEnum, ::testing::Values(999));

TEST(LoggingRunSnapshot, RejectsInvalidRunInputs)
{
    const auto values = Model({.parameters = {Parameter("rpm")}}, {"rpm"});
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "", Policy(), LoggingTarget::kEcu),
                IsErr(ErrorKind::kInvalidConfig));
    auto bad_policy = Policy();
    bad_policy.poll_timeout = 0ms;
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", bad_policy, LoggingTarget::kEcu),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("SSM")));
}
} // namespace
} // namespace fastecu::logging
