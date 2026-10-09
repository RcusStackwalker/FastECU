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

LoggingPolicy policy()
{
    return {.poll_timeout = 100ms,
            .car_silence_miss_threshold = 3,
            .reconnect_attempt_threshold = 5,
            .reconnect_retry_period = 10};
}
LoggerParameter parameter(std::string id, std::string protocol = "SSM", bool enabled = true)
{
    return {.protocol = std::move(protocol),
            .id = std::move(id),
            .address = "000010",
            .length = "1",
            .enabled = enabled,
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}
LoggerModel model(LoggerDefinition definition, std::vector<std::string> ids)
{
    LoggerModel result;
    result.install_definition(std::move(definition));
    result.set_selection({.protocol = "SSM", .lower_panel_ids = std::move(ids)});
    return result;
}

TEST(LoggingRunSnapshot, OwnsMeasurementPresentationAfterModelDestruction)
{
    const auto run = []
    {
        auto rpm = parameter("rpm");
        rpm.name = "Engine RPM";
        const auto source = model({.parameters = {rpm}}, {"rpm"});
        return prepare_logging_run(source, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu);
    }();
    ASSERT_THAT(run, IsOk());
    const auto *measurement = run->find_measurement(LoggingMeasurementKind::Parameter, "rpm");
    ASSERT_NE(measurement, nullptr);
    EXPECT_EQ(measurement->identity, (LoggerIdentity{"SSM", "rpm"}));
    EXPECT_EQ(measurement->name, "Engine RPM");
    EXPECT_EQ(measurement->unit, "rpm");
    EXPECT_EQ(measurement->decimal_precision, 2);
    EXPECT_EQ(measurement->support, EcuSupport::Unknown);
}

TEST(LoggingRunSnapshot, AcquiresSelectedUnionAndSharesRepeatedParameterIdentity)
{
    auto values = model({.parameters = {parameter("rpm")},
                         .switches = {{.protocol = "SSM", .id = "rpm", .address = "20", .sample_bit = "7"}}},
                        {});
    values.set_selection(
        {.protocol = "SSM", .gauge_ids = {"rpm"}, .lower_panel_ids = {"rpm", "rpm"}, .switch_ids = {"rpm"}});
    const auto run = prepare_logging_run(values, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu);
    ASSERT_THAT(run, IsOk());
    ASSERT_EQ(run->session().channels().size(), 2U);
    ASSERT_NE(run->find_measurement(LoggingMeasurementKind::Parameter, "rpm"), nullptr);
    ASSERT_NE(run->find_measurement(LoggingMeasurementKind::Switch, "rpm"), nullptr);
    EXPECT_EQ(run->session().channels()[0].id, "parameter:rpm");
    EXPECT_EQ(run->session().channels()[1].id, "switch:rpm");
}
TEST(LoggingRunSnapshot, RejectsKnownUnsupportedSelectedMeasurement)
{
    auto values = model({.parameters = {parameter("rpm")}}, {"rpm"});
    values.set_parameter_support("SSM", "rpm", EcuSupport::Unsupported);
    EXPECT_THAT(prepare_logging_run(values, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("unsupported"))));
}
TEST(LoggingRunSnapshot, ExplicitUnknownMutSelectionIsValidatedEvenWhenXmlDisabled)
{
    const auto values = model({.parameters = {parameter("rpm", "MUT_DMA", false)}}, {"rpm"});
    const auto run = prepare_logging_run(values, LoggingProtocolId::MutDma, "ignored", policy(), LoggingTarget::Ecu);
    ASSERT_THAT(run, IsOk());
    ASSERT_EQ(run->session().channels().size(), 1U);
}

TEST(LoggingRunSnapshot, AcquiresGaugeOnlyAndSwitchOnlySelections)
{
    auto values = model({.parameters = {parameter("rpm")},
                         .switches = {{.protocol = "SSM", .id = "flag", .address = "20", .sample_bit = "5"}}},
                        {});
    values.set_selection({.protocol = "SSM", .gauge_ids = {"rpm"}});
    const auto gauge = prepare_logging_run(values, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Tcu);
    ASSERT_THAT(gauge, IsOk());
    ASSERT_EQ(gauge->session().channels().size(), 1U);
    EXPECT_EQ(gauge->target(), LoggingTarget::Tcu);
    values.set_selection({.protocol = "SSM", .switch_ids = {"flag"}});
    const auto flag = prepare_logging_run(values, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu);
    ASSERT_THAT(flag, IsOk());
    ASSERT_EQ(flag->session().channels().size(), 1U);
    EXPECT_EQ(flag->session().channels()[0].sample_bit, 5);
}

TEST(LoggingRunSnapshot, ValidatesOnlyParticipatingChannels)
{
    for (const auto& [protocol, key] :
         std::vector<std::pair<LoggingProtocolId, std::string>>{{LoggingProtocolId::Ssm, "SSM"},
                                                                {LoggingProtocolId::MutDma, "MUT_DMA"},
                                                                {LoggingProtocolId::Cdbg, "CDBG"}})
    {
        SCOPED_TRACE(key);
        auto invalid = parameter("off", key, false);
        invalid.conversions.front().format = "banana";
        const auto values = model({.parameters = {invalid, parameter("on", key)}}, {"off", "on"});
        const auto result = prepare_logging_run(values, protocol, key, policy(), LoggingTarget::Ecu);
        EXPECT_THAT(result,
                    IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr(key), HasSubstr("off"), HasSubstr("format"))));
    }
}

TEST(LoggingRunSnapshot, CapturesOwnedInputs)
{
    auto snapshot = []
    {
        auto source = model({.parameters = {parameter("rpm"), parameter("rpm", "CDBG"), parameter("coolant")}},
                            {"rpm", "coolant"});
        auto result = prepare_logging_run(source, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Tcu);
        source.set_selection({.protocol = "CDBG", .lower_panel_ids = {"rpm"}});
        source.set_parameter_support("SSM", "rpm", fastecu::logging::EcuSupport::Unsupported);
        return result;
    }();
    ASSERT_THAT(snapshot, IsOk());
    EXPECT_EQ(snapshot->protocol_key(), "SSM");
    EXPECT_EQ(snapshot->target(), LoggingTarget::Tcu);
    ASSERT_NE(snapshot->find_measurement(LoggingMeasurementKind::Parameter, "rpm"), nullptr);
    EXPECT_THAT(snapshot->selection().lower_panel_ids, ElementsAre("rpm", "coolant"));
    ASSERT_EQ(snapshot->session().channels().size(), 2U);
    EXPECT_EQ(snapshot->session().channels()[0].id, "parameter:rpm");
    const auto copy = *snapshot;
    EXPECT_EQ(copy.target(), LoggingTarget::Tcu);
}

TEST(LoggingRunSnapshot, RejectsDuplicateParticipatingIds)
{
    const auto duplicate_definition = model({.parameters = {parameter("rpm"), parameter("rpm")}}, {"rpm"});
    EXPECT_THAT(prepare_logging_run(duplicate_definition, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr("duplicate"))));
    const auto duplicate_selection = model({.parameters = {parameter("rpm")}}, {"rpm", "rpm"});
    const auto repeated =
        prepare_logging_run(duplicate_selection, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu);
    ASSERT_THAT(repeated, IsOk());
    EXPECT_EQ(repeated->session().channels().size(), 1U);
}

TEST(LoggingRunSnapshot, RejectsUnresolvedSelectionWithActionableIdentity)
{
    const auto values = model({}, {"missing"});
    for (const auto protocol : {LoggingProtocolId::Ssm, LoggingProtocolId::MutDma, LoggingProtocolId::Cdbg})
    {
        EXPECT_THAT(prepare_logging_run(values, protocol, "SSM", policy(), LoggingTarget::Ecu),
                    IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("missing"), HasSubstr("unresolved"))));
    }
}
TEST(LoggingRunSnapshot, CapturesExplicitMutDialectAndRejectsUnknownSetting)
{
    auto values =
        model({.parameters = {parameter("rpm", "MUT_DMA")}, .protocols = {{"MUT_DMA", "oem-33520003"}}}, {"rpm"});
    const auto run = prepare_logging_run(values, LoggingProtocolId::MutDma, "ignored", policy(), LoggingTarget::Ecu);
    ASSERT_THAT(run, IsOk());
    EXPECT_EQ(run->session().mut_dma_dialect(), mutdma::FreeformDialect::Oem33520003);
    values = model({.parameters = {parameter("rpm", "MUT_DMA")}, .protocols = {{"MUT_DMA", "unknown"}}}, {"rpm"});
    EXPECT_THAT(prepare_logging_run(values, LoggingProtocolId::MutDma, "ignored", policy(), LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("unknown dialect")));
}

class LoggingRunSnapshotInvalidEnum : public ::testing::TestWithParam<int>
{
};

TEST_P(LoggingRunSnapshotInvalidEnum, RejectsUnknownProtocolAndTarget)
{
    const auto values = model({.parameters = {parameter("rpm")}}, {"rpm"});
    const auto input = GetParam();
    EXPECT_THAT(prepare_logging_run(values, static_cast<LoggingProtocolId>(input), "SSM", policy(), LoggingTarget::Ecu),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(prepare_logging_run(values, LoggingProtocolId::Ssm, "SSM", policy(), static_cast<LoggingTarget>(input)),
                IsErr(ErrorKind::InvalidConfig));
}

INSTANTIATE_TEST_SUITE_P(UnknownInputs, LoggingRunSnapshotInvalidEnum, ::testing::Values(999));

TEST(LoggingRunSnapshot, RejectsInvalidRunInputs)
{
    const auto values = model({.parameters = {parameter("rpm")}}, {"rpm"});
    EXPECT_THAT(prepare_logging_run(values, LoggingProtocolId::Ssm, "", policy(), LoggingTarget::Ecu),
                IsErr(ErrorKind::InvalidConfig));
    auto bad_policy = policy();
    bad_policy.poll_timeout = 0ms;
    EXPECT_THAT(prepare_logging_run(values, LoggingProtocolId::Ssm, "SSM", bad_policy, LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("SSM")));
}
} // namespace
} // namespace fastecu::logging
