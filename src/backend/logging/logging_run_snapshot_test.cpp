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
LoggerParameter BuildParameter(std::string id, std::string protocol = "SSM", bool enabled = true)
{
    return {.protocol = std::move(protocol),
            .id = std::move(id),
            .address = "000010",
            .length = "1",
            .enabled = enabled,
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}
LoggerModel BuildModel(LoggerDefinition definition, std::vector<std::string> ids)
{
    LoggerModel result;
    result.InstallDefinition(std::move(definition));
    result.SetSelection({.protocol = "SSM", .lower_panel_ids = std::move(ids)});
    return result;
}

TEST(LoggingRunSnapshot, PreservesProtocolSelectionSupportAndOriginalOffsets)
{
    const auto values =
        BuildModel({.parameters = {BuildParameter("other", "OTHER"), BuildParameter("off", "CAR_SSM", false),
                                   BuildParameter("on", "CAR_SSM"), BuildParameter("mut-off", "MUT_DMA", false),
                                   BuildParameter("mut-on", "MUT_DMA"), BuildParameter("cdbg-off", "CDBG", false)}},
                   {"other", "missing", "off", "on", "mut-off", "mut-on", "cdbg-off"});
    const auto ssm = PrepareLoggingRun(values, LoggingProtocolId::kSsm, "CAR_SSM", Policy(), LoggingTarget::kTcu);
    const auto mut = PrepareLoggingRun(values, LoggingProtocolId::kMutDma, "ignored", Policy(), LoggingTarget::kEcu);
    const auto cdbg = PrepareLoggingRun(values, LoggingProtocolId::kCdbg, "ignored", Policy(), LoggingTarget::kEcu);
    ASSERT_THAT(ssm, IsOk());
    ASSERT_THAT(mut, IsOk());
    ASSERT_THAT(cdbg, IsOk());
    EXPECT_EQ(ssm->ProtocolKey(), "CAR_SSM");
    EXPECT_THAT(ssm->ResponseOffsets(), ElementsAre(2, 3));
    EXPECT_FALSE(ssm->ChannelEnabled("off"));
    EXPECT_TRUE(ssm->ChannelEnabled("on"));
    ASSERT_EQ(ssm->Session().Channels().size(), 2U);
    EXPECT_EQ(ssm->Session().Channels()[0].id, "off");
    EXPECT_EQ(ssm->Session().Channels()[1].id, "on");
    EXPECT_EQ(ssm->Session().Channels()[0].raw_assembly, RawAssembly::kDecimalBytesConcatenated);
    EXPECT_EQ(mut->ProtocolKey(), "MUT_DMA");
    ASSERT_EQ(mut->Session().Channels().size(), 1U);
    EXPECT_EQ(mut->Session().Channels()[0].id, "mut-on");
    EXPECT_EQ(mut->Session().Channels()[0].raw_assembly, RawAssembly::kUnsignedIntegerDecimal);
    EXPECT_TRUE(mut->ResponseOffsets().empty());
    EXPECT_EQ(cdbg->ProtocolKey(), "CDBG");
    ASSERT_EQ(cdbg->Session().Channels().size(), 1U);
    EXPECT_TRUE(cdbg->ChannelEnabled("cdbg-off"));
}

TEST(LoggingRunSnapshot, ValidatesOnlyParticipatingChannels)
{
    for (const auto& [protocol, key] :
         std::vector<std::pair<LoggingProtocolId, std::string>>{{LoggingProtocolId::kSsm, "SSM"},
                                                                {LoggingProtocolId::kMutDma, "MUT_DMA"},
                                                                {LoggingProtocolId::kCdbg, "CDBG"}})
    {
        SCOPED_TRACE(key);
        auto invalid = BuildParameter("off", key, false);
        invalid.conversions.front().format = "banana";
        const auto values = BuildModel({.parameters = {invalid, BuildParameter("on", key)}}, {"off", "on"});
        const auto result = PrepareLoggingRun(values, protocol, key, Policy(), LoggingTarget::kEcu);
        if (protocol == LoggingProtocolId::kMutDma)
        {
            ASSERT_THAT(result, IsOk());
            ASSERT_EQ(result->Session().Channels().size(), 1U);
            EXPECT_EQ(result->Session().Channels()[0].id, "on");
        }
        else
        {
            EXPECT_THAT(result, IsErrWith(ErrorKind::kInvalidConfig,
                                          AllOf(HasSubstr(key), HasSubstr("off"), HasSubstr("format"))));
        }
    }
}

TEST(LoggingRunSnapshot, CapturesOwnedInputs)
{
    auto snapshot = []
    {
        auto source = BuildModel(
            {.parameters = {BuildParameter("rpm"), BuildParameter("rpm", "CDBG"), BuildParameter("coolant")}},
            {"rpm", "coolant"});
        auto result = PrepareLoggingRun(source, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kTcu);
        source.SetSelection({.protocol = "CDBG", .lower_panel_ids = {"rpm"}});
        source.SetParameterSupported("SSM", "rpm", false);
        return result;
    }();
    ASSERT_THAT(snapshot, IsOk());
    EXPECT_EQ(snapshot->ProtocolKey(), "SSM");
    EXPECT_EQ(snapshot->Target(), LoggingTarget::kTcu);
    EXPECT_TRUE(snapshot->ChannelEnabled("rpm"));
    EXPECT_THAT(snapshot->Selection().lower_panel_ids, ElementsAre("rpm", "coolant"));
    ASSERT_EQ(snapshot->Session().Channels().size(), 2U);
    EXPECT_EQ(snapshot->Session().Channels()[0].id, "rpm");
    const auto copy = *snapshot;
    EXPECT_EQ(copy.Target(), LoggingTarget::kTcu);
}

TEST(LoggingRunSnapshot, RejectsDuplicateParticipatingIds)
{
    const auto duplicate_definition =
        BuildModel({.parameters = {BuildParameter("rpm"), BuildParameter("rpm")}}, {"rpm"});
    EXPECT_THAT(
        PrepareLoggingRun(duplicate_definition, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu),
        IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr("duplicate"))));
    const auto duplicate_selection = BuildModel({.parameters = {BuildParameter("rpm")}}, {"rpm", "rpm"});
    EXPECT_THAT(
        PrepareLoggingRun(duplicate_selection, LoggingProtocolId::kSsm, "SSM", Policy(), LoggingTarget::kEcu),
        IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr("duplicate"))));
}

TEST(LoggingRunSnapshot, RetainsEmptyAndUnresolvedSelectionBehavior)
{
    const auto values = BuildModel({}, {"missing"});
    for (const auto protocol : {LoggingProtocolId::kSsm, LoggingProtocolId::kMutDma})
    {
        SCOPED_TRACE(static_cast<int>(protocol));
        const auto result = PrepareLoggingRun(values, protocol, "SSM", Policy(), LoggingTarget::kEcu);
        ASSERT_THAT(result, IsOk());
        EXPECT_TRUE(result->Session().Channels().empty());
        EXPECT_THAT(result->Selection().lower_panel_ids, ElementsAre("missing"));
    }
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kCdbg, "SSM", Policy(), LoggingTarget::kEcu),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("CDBG")));
}

class LoggingRunSnapshotInvalidEnum : public ::testing::TestWithParam<int>
{
};

TEST_P(LoggingRunSnapshotInvalidEnum, RejectsUnknownProtocolAndTarget)
{
    const auto values = BuildModel({.parameters = {BuildParameter("rpm")}}, {"rpm"});
    const auto input = GetParam();
    EXPECT_THAT(PrepareLoggingRun(values, static_cast<LoggingProtocolId>(input), "SSM", Policy(), LoggingTarget::kEcu),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", Policy(), static_cast<LoggingTarget>(input)),
                IsErr(ErrorKind::kInvalidConfig));
}

INSTANTIATE_TEST_SUITE_P(UnknownInputs, LoggingRunSnapshotInvalidEnum, ::testing::Values(999));

TEST(LoggingRunSnapshot, RejectsInvalidRunInputs)
{
    const auto values = BuildModel({.parameters = {BuildParameter("rpm")}}, {"rpm"});
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "", Policy(), LoggingTarget::kEcu),
                IsErr(ErrorKind::kInvalidConfig));
    auto bad_policy = Policy();
    bad_policy.poll_timeout = 0ms;
    EXPECT_THAT(PrepareLoggingRun(values, LoggingProtocolId::kSsm, "SSM", bad_policy, LoggingTarget::kEcu),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("SSM")));
}
} // namespace
} // namespace fastecu::logging
