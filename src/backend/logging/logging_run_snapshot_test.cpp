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

TEST(LoggingRunSnapshot, PreservesProtocolSelectionSupportAndOriginalOffsets)
{
    const auto values = model({.parameters = {parameter("other", "OTHER"), parameter("off", "CAR_SSM", false),
                                              parameter("on", "CAR_SSM"), parameter("mut-off", "MUT_DMA", false),
                                              parameter("mut-on", "MUT_DMA"), parameter("cdbg-off", "CDBG", false)}},
                              {"other", "missing", "off", "on", "mut-off", "mut-on", "cdbg-off"});
    const auto ssm = prepare_logging_run(values, LoggingProtocolId::Ssm, "CAR_SSM", policy(), LoggingTarget::Tcu);
    const auto mut = prepare_logging_run(values, LoggingProtocolId::MutDma, "ignored", policy(), LoggingTarget::Ecu);
    const auto cdbg = prepare_logging_run(values, LoggingProtocolId::Cdbg, "ignored", policy(), LoggingTarget::Ecu);
    ASSERT_THAT(ssm, IsOk());
    ASSERT_THAT(mut, IsOk());
    ASSERT_THAT(cdbg, IsOk());
    EXPECT_EQ(ssm->protocol_key(), "CAR_SSM");
    EXPECT_THAT(ssm->response_offsets(), ElementsAre(2, 3));
    EXPECT_FALSE(ssm->channel_enabled("off"));
    EXPECT_TRUE(ssm->channel_enabled("on"));
    ASSERT_EQ(ssm->session().channels().size(), 2U);
    EXPECT_EQ(ssm->session().channels()[0].id, "off");
    EXPECT_EQ(ssm->session().channels()[1].id, "on");
    EXPECT_EQ(ssm->session().channels()[0].raw_assembly, RawAssembly::DecimalBytesConcatenated);
    EXPECT_EQ(mut->protocol_key(), "MUT_DMA");
    ASSERT_EQ(mut->session().channels().size(), 1U);
    EXPECT_EQ(mut->session().channels()[0].id, "mut-on");
    EXPECT_EQ(mut->session().channels()[0].raw_assembly, RawAssembly::UnsignedIntegerDecimal);
    EXPECT_TRUE(mut->response_offsets().empty());
    EXPECT_EQ(cdbg->protocol_key(), "CDBG");
    ASSERT_EQ(cdbg->session().channels().size(), 1U);
    EXPECT_TRUE(cdbg->channel_enabled("cdbg-off"));
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
        if (protocol == LoggingProtocolId::MutDma)
        {
            ASSERT_THAT(result, IsOk());
            ASSERT_EQ(result->session().channels().size(), 1U);
            EXPECT_EQ(result->session().channels()[0].id, "on");
        }
        else
        {
            EXPECT_THAT(result, IsErrWith(ErrorKind::InvalidConfig,
                                          AllOf(HasSubstr(key), HasSubstr("off"), HasSubstr("format"))));
        }
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
        source.set_parameter_supported("SSM", "rpm", false);
        return result;
    }();
    ASSERT_THAT(snapshot, IsOk());
    EXPECT_EQ(snapshot->protocol_key(), "SSM");
    EXPECT_EQ(snapshot->target(), LoggingTarget::Tcu);
    EXPECT_TRUE(snapshot->channel_enabled("rpm"));
    EXPECT_THAT(snapshot->selection().lower_panel_ids, ElementsAre("rpm", "coolant"));
    ASSERT_EQ(snapshot->session().channels().size(), 2U);
    EXPECT_EQ(snapshot->session().channels()[0].id, "rpm");
    const auto copy = *snapshot;
    EXPECT_EQ(copy.target(), LoggingTarget::Tcu);
}

TEST(LoggingRunSnapshot, RejectsDuplicateParticipatingIds)
{
    const auto duplicate_definition = model({.parameters = {parameter("rpm"), parameter("rpm")}}, {"rpm"});
    EXPECT_THAT(prepare_logging_run(duplicate_definition, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr("duplicate"))));
    const auto duplicate_selection = model({.parameters = {parameter("rpm")}}, {"rpm", "rpm"});
    EXPECT_THAT(prepare_logging_run(duplicate_selection, LoggingProtocolId::Ssm, "SSM", policy(), LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr("duplicate"))));
}

TEST(LoggingRunSnapshot, RetainsEmptyAndUnresolvedSelectionBehavior)
{
    const auto values = model({}, {"missing"});
    for (const auto protocol : {LoggingProtocolId::Ssm, LoggingProtocolId::MutDma})
    {
        SCOPED_TRACE(static_cast<int>(protocol));
        const auto result = prepare_logging_run(values, protocol, "SSM", policy(), LoggingTarget::Ecu);
        ASSERT_THAT(result, IsOk());
        EXPECT_TRUE(result->session().channels().empty());
        EXPECT_THAT(result->selection().lower_panel_ids, ElementsAre("missing"));
    }
    EXPECT_THAT(prepare_logging_run(values, LoggingProtocolId::Cdbg, "SSM", policy(), LoggingTarget::Ecu),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("CDBG")));
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
