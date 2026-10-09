#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logging_session.h"

#include <chrono>

#include <gtest/gtest.h>

namespace
{

using namespace fastecu::logging;
using namespace std::chrono_literals;

LoggingChannel Channel(std::string id, std::uint32_t address)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = 2,
        .raw_assembly = RawAssembly::kDecimalBytesConcatenated,
        .from_byte_expression = "x",
        .unit = "rpm",
        .decimal_precision = 2,
    };
}

std::vector<LoggingChannel> ValidChannels()
{
    return {Channel("rpm", 0x10)};
}

LoggingPolicy ValidPolicy()
{
    return LoggingPolicy{
        .poll_timeout = 100ms,
        .car_silence_miss_threshold = 3,
        .reconnect_attempt_threshold = 2,
        .reconnect_retry_period = 0,
    };
}

} // namespace

TEST(LoggingSessionTest, RejectsDuplicateStableIds)
{
    auto channels = ValidChannels();
    channels.push_back(channels.front());
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, channels, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, StableIdsSurviveSourceRowReordering)
{
    auto result =
        MakeLoggingSession(LoggingProtocolId::kSsm, {Channel("rpm", 0x10), Channel("coolant", 0x20)}, ValidPolicy());
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->Channels()[0].id, "rpm");
    EXPECT_EQ(result->Channels()[1].id, "coolant");
    EXPECT_EQ(result->FindChannel("coolant")->address, 0x20U);
}

TEST(LoggingSessionTest, RejectsInvalidPolicy)
{
    auto policy = ValidPolicy();
    policy.poll_timeout = 0ms;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, ValidChannels(), policy),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, RejectsInvalidChannelShape)
{
    auto c = Channel("rpm", 0x10);
    c.length = 0;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, RejectsInvalidChannelIdentityAndAssembly)
{
    auto c = Channel("", 0x10);
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c = Channel("rpm", 0x10);
    c.length = 256;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c = Channel("rpm", 0x10);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- exercises the invalid-value rejection path
    c.raw_assembly = static_cast<RawAssembly>(99);
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, RejectsOutOfRangeProtocolAddresses)
{
    auto c = Channel("rpm", 0x1000000);
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.address = 0x10000;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, {c}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.address = 0xffffffff;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, {c}, ValidPolicy()), fastecu::testing::IsOk());
}

TEST(LoggingSessionTest, RejectsInvalidConversionConfiguration)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression.clear();
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.from_byte_expression = "x+invalid";
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.from_byte_expression = "x";
    c.decimal_precision = 16;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));
}

TEST(LoggingSessionTest, RejectsExpressionsWithoutFiniteEvaluation)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression = "x/0";
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c.from_byte_expression = "0/0";
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, RequiresAtLeastOneCdbgChannel)
{
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, {}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {}, ValidPolicy()), fastecu::testing::IsOk());

    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, {}, ValidPolicy()), fastecu::testing::IsOk());
}

TEST(LoggingSessionTest, RejectsUnknownProtocolIdentifiers)
{
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- exercising the invalid-value rejection path
    ASSERT_THAT(MakeLoggingSession(static_cast<LoggingProtocolId>(99), {}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, RejectsProtocolSpecificWireShapesBeforeIo)
{
    auto ssm_channel = Channel("ssm", 0x10);
    ssm_channel.length = 256;
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {ssm_channel}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    auto mut_channel = Channel("mut", 0x10);
    mut_channel.length = 3;
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, {mut_channel}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    auto cdbg_channel = Channel("cdbg", 0x804000);
    cdbg_channel.length = 3;
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, {cdbg_channel}, ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    std::vector<LoggingChannel> too_many_cdbg_channels;
    for (int i = 0; i < 57; ++i)
    {
        too_many_cdbg_channels.push_back(Channel("cdbg-" + std::to_string(i), 0x804000 + i));
        too_many_cdbg_channels.back().length = 1;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, std::move(too_many_cdbg_channels), ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    std::vector<LoggingChannel> too_many_ssm_channels;
    for (int i = 0; i < 85; ++i)
    {
        too_many_ssm_channels.push_back(Channel("ssm-" + std::to_string(i), i));
        too_many_ssm_channels.back().length = 1;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, std::move(too_many_ssm_channels), ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));

    std::vector<LoggingChannel> too_many_mut_channels;
    for (int i = 0; i < 256; ++i)
    {
        too_many_mut_channels.push_back(Channel("mut-" + std::to_string(i), i));
        too_many_mut_channels.back().length = 1;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, std::move(too_many_mut_channels), ValidPolicy()),
                ::testing::Not(fastecu::testing::IsOk()));
}

TEST(LoggingChannelValidation, SharesExistingBoundsAndExpressionProbes)
{
    auto channel = Channel("rpm", 0x10);
    EXPECT_THAT(ValidateLoggingChannel(LoggingProtocolId::kSsm, channel), fastecu::testing::IsOk());
    channel.address = 0x1000000;
    EXPECT_THAT(ValidateLoggingChannel(LoggingProtocolId::kSsm, channel),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    channel.address = 0x10;
    channel.from_byte_expression = "1/(x-1)";
    EXPECT_THAT(ValidateLoggingChannel(LoggingProtocolId::kSsm, channel), fastecu::testing::IsOk());
    channel.from_byte_expression = "+(x*1e2)";
    EXPECT_THAT(ValidateLoggingChannel(LoggingProtocolId::kSsm, channel), fastecu::testing::IsOk());
    channel.from_byte_expression = "x+";
    EXPECT_THAT(ValidateLoggingChannel(LoggingProtocolId::kSsm, channel),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}
