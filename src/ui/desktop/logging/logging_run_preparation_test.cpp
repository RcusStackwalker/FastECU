#include "src/ui/desktop/logging/logging_run_preparation.h"

#include <chrono>
#include <string>
#include <tuple>
#include <utility>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/logging/logger_model.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::ui
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using logging::LoggingProtocolId;
using logging::LoggingTarget;
using ::testing::ElementsAre;

logging::LoggerModel modelFor(std::string filter)
{
    logging::LoggerModel model;
    model.InstallDefinition({.parameters = {{.protocol = std::move(filter),
                                             .id = "rpm",
                                             .name = "Engine RPM",
                                             .address = "10",
                                             .length = "1",
                                             .enabled = true,
                                             .conversions = {{.units = "rpm", .expr = "x", .format = "0"}}}}});
    model.SetSelection({.protocol = "pending", .lower_panel_ids = {"rpm"}});
    return model;
}

struct RoutingCase
{
    const char *name;
    const char *saved_protocol;
    const char *ssm_filter;
    const char *registration_key;
    LoggingProtocolId protocol;
    const char *definition_filter;
    std::chrono::milliseconds poll_timeout;
    int silence_threshold;
    int reconnect_threshold;
    int retry_period;
};

class LoggingRunPreparation : public ::testing::TestWithParam<std::tuple<RoutingCase, LoggingTarget>>
{
};

std::string routingCaseName(const ::testing::TestParamInfo<LoggingRunPreparation::ParamType>& info)
{
    const auto& [route, target] = info.param;
    return std::string(route.name) + (target == LoggingTarget::kEcu ? "Ecu" : "Tcu");
}

TEST_P(LoggingRunPreparation, CapturesMatchingAcquisitionAndDefinitionChoices)
{
    const auto& [route, target] = GetParam();
    const auto model = modelFor(route.definition_filter);
    const auto prepared = prepareLoggingRun(
        model, {.saved_protocol = route.saved_protocol, .ssm_definition_filter = route.ssm_filter, .target = target});
    ASSERT_THAT(prepared, IsOk());
    EXPECT_EQ(prepared->registration_key, route.registration_key);
    EXPECT_EQ(prepared->snapshot.Session().Protocol(), route.protocol);
    EXPECT_EQ(prepared->snapshot.ProtocolKey(), route.definition_filter);
    EXPECT_EQ(prepared->snapshot.Target(), target);
    EXPECT_EQ(prepared->snapshot.Selection().protocol, "pending");
    EXPECT_THAT(prepared->snapshot.Selection().lower_panel_ids, ElementsAre("rpm"));
    const auto& policy = prepared->snapshot.Session().Policy();
    EXPECT_EQ(policy.poll_timeout, route.poll_timeout);
    EXPECT_EQ(policy.car_silence_miss_threshold, route.silence_threshold);
    EXPECT_EQ(policy.reconnect_attempt_threshold, route.reconnect_threshold);
    EXPECT_EQ(policy.reconnect_retry_period, route.retry_period);
}

INSTANTIATE_TEST_SUITE_P(
    ProtocolChoices, LoggingRunPreparation,
    ::testing::Combine(
        ::testing::Values(
            RoutingCase{"Mut", "MUT_DMA", "", "MUT_DMA", LoggingProtocolId::kMutDma, "MUT_DMA", 50ms, 20, 100, 20},
            RoutingCase{"Cdbg", "CDBG", "", "CDBG", LoggingProtocolId::kCdbg, "CDBG", 50ms, 20, 100, 20},
            RoutingCase{"Ssm", "SSM", "CUSTOM", "SSM", LoggingProtocolId::kSsm, "CUSTOM", 300ms, 10, 30, 10},
            RoutingCase{"Unknown", "unrecognized", "CUSTOM", "SSM", LoggingProtocolId::kSsm, "CUSTOM", 300ms, 10, 30,
                        10},
            RoutingCase{"Empty", "", "CUSTOM", "SSM", LoggingProtocolId::kSsm, "CUSTOM", 300ms, 10, 30, 10},
            RoutingCase{"Lowercase", "cdbg", "CUSTOM", "SSM", LoggingProtocolId::kSsm, "CUSTOM", 300ms, 10, 30, 10},
            RoutingCase{"Padded", " MUT_DMA ", "CUSTOM", "SSM", LoggingProtocolId::kSsm, "CUSTOM", 300ms, 10, 30, 10}),
        ::testing::Values(LoggingTarget::kEcu, LoggingTarget::kTcu)),
    routingCaseName);

class LoggingRunPreparationInvalidTarget : public ::testing::TestWithParam<int>
{
};

TEST_P(LoggingRunPreparationInvalidTarget, EmptySsmFilterWinsOverInvalidTarget)
{
    const auto model = modelFor("CUSTOM");
    EXPECT_THAT(prepareLoggingRun(model, {.saved_protocol = "unknown",
                                          .ssm_definition_filter = "",
                                          .target = static_cast<LoggingTarget>(GetParam())}),
                IsErrWith(ErrorKind::kInvalidConfig, "SSM logging protocol filter is empty"));
}

TEST_P(LoggingRunPreparationInvalidTarget, PreservesInvalidTargetDetail)
{
    const auto model = modelFor("CUSTOM");
    EXPECT_THAT(prepareLoggingRun(model, {.saved_protocol = "SSM",
                                          .ssm_definition_filter = "CUSTOM",
                                          .target = static_cast<LoggingTarget>(GetParam())}),
                IsErrWith(ErrorKind::kInvalidConfig, "CUSTOM: invalid logging target"));
}

INSTANTIATE_TEST_SUITE_P(UnknownTarget, LoggingRunPreparationInvalidTarget, ::testing::Values(99),
                         [](const auto&) { return "UnknownTargetValue"; });

TEST(LoggingRunPreparationErrors, PreservesUnsupportedSelectionDetail)
{
    auto model = modelFor("CUSTOM");
    model.SetParameterSupport("CUSTOM", "rpm", logging::EcuSupport::kUnsupported);
    EXPECT_THAT(prepareLoggingRun(
                    model, {.saved_protocol = "SSM", .ssm_definition_filter = "CUSTOM", .target = LoggingTarget::kEcu}),
                IsErrWith(ErrorKind::kInvalidConfig,
                          "CUSTOM parameter:rpm: selected measurement is known unsupported; remove or replace it"));
}

TEST(LoggingRunPreparationCapture, OwnsChoicesAndSnapshotAfterInputsChange)
{
    const auto prepared = []
    {
        std::string savedProtocol = "unknown";
        std::string definitionFilter = "CUSTOM";
        auto model = modelFor(definitionFilter);
        auto result = prepareLoggingRun(model, {.saved_protocol = savedProtocol,
                                                .ssm_definition_filter = definitionFilter,
                                                .target = LoggingTarget::kTcu});
        savedProtocol.assign("CDBG");
        definitionFilter.assign("changed");
        model.SetSelection({.protocol = "changed", .lower_panel_ids = {"missing"}});
        model.SetParameterSupport("CUSTOM", "rpm", logging::EcuSupport::kUnsupported);
        return result;
    }();
    ASSERT_THAT(prepared, IsOk());
    EXPECT_EQ(prepared->registration_key, "SSM");
    EXPECT_EQ(prepared->snapshot.ProtocolKey(), "CUSTOM");
    EXPECT_EQ(prepared->snapshot.Target(), LoggingTarget::kTcu);
    EXPECT_THAT(prepared->snapshot.Selection().lower_panel_ids, ElementsAre("rpm"));
    ASSERT_EQ(prepared->snapshot.Measurements().size(), 1U);
    EXPECT_EQ(prepared->snapshot.Measurements().front().name, "Engine RPM");
    EXPECT_EQ(prepared->snapshot.Measurements().front().support, logging::EcuSupport::kUnknown);
}
} // namespace
} // namespace fastecu::ui
