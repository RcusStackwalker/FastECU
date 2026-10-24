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

TEST(LoggingSessionTest, RejectsInvalidChannelIdentityAndNumericStorage)
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
    c.storage = static_cast<NumericStorage>(99);
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, RejectsOutOfRangeProtocolAddresses)
{
    auto c = Channel("rpm", 0x1000000);
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c.address = 0x10000;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c.address = 0xffffffff;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, {c}, ValidPolicy()), fastecu::testing::IsOk());
}

TEST(LoggingSessionTest, RejectsInvalidConversionConfiguration)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression.clear();
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c.from_byte_expression = "x+invalid";
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    c.from_byte_expression = "x";
    c.decimal_precision = 16;
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {c}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
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
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    auto mut_channel = Channel("mut", 0x10);
    mut_channel.length = 3;
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, {mut_channel}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    auto cdbg_channel = Channel("cdbg", 0x804000);
    cdbg_channel.length = 3;
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, {cdbg_channel}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    std::vector<LoggingChannel> too_many_cdbg_channels;
    for (int i = 0; i < 57; ++i)
    {
        too_many_cdbg_channels.push_back(
            Channel("cdbg-" + std::to_string(i), static_cast<std::uint32_t>(0x804000 + i)));
        too_many_cdbg_channels.back().length = 1;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kCdbg, std::move(too_many_cdbg_channels), ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    std::vector<LoggingChannel> too_many_ssm_channels;
    for (int i = 0; i < 85; ++i)
    {
        too_many_ssm_channels.push_back(Channel("ssm-" + std::to_string(i), static_cast<std::uint32_t>(i)));
        too_many_ssm_channels.back().length = 1;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, std::move(too_many_ssm_channels), ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));

    std::vector<LoggingChannel> too_many_mut_channels;
    for (int i = 0; i < 256; ++i)
    {
        too_many_mut_channels.push_back(Channel("mut-" + std::to_string(i), static_cast<std::uint32_t>(i)));
        too_many_mut_channels.back().length = 1;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, std::move(too_many_mut_channels), ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, SsmCapacityCountsPhysicalBytes)
{
    std::vector<LoggingChannel> channels;
    for (int i = 0; i < 21; ++i)
    {
        auto source = Channel(std::to_string(i), static_cast<std::uint32_t>(i * 4));
        source.length = 4;
        channels.push_back(source);
    }
    ASSERT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, channels, ValidPolicy()), fastecu::testing::IsOk());
    auto extra = Channel("extra", 0x100);
    extra.length = 4;
    channels.push_back(extra);
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, channels, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}
TEST(LoggingSessionTest, SsmRejectsMultiByteAddressOverflow)
{
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {Channel("wide", 0xffffff)}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingSessionTest, MutBoundsEntriesAndOutputBytes)
{
    std::vector<LoggingChannel> sources;
    for (int i = 0; i < 96; ++i)
    {
        auto source = Channel(std::to_string(i), static_cast<std::uint32_t>(0x4000 + i));
        source.length = 1;
        sources.push_back(source);
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, sources, ValidPolicy()), fastecu::testing::IsOk());
    sources[0].length = 2;
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, sources, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    sources[0].length = 1;
    sources.push_back(Channel("overflow", 0x100));
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kMutDma, sources, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
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

TEST(LoggingSessionTest, BoundsExpandedSsmByteRequests)
{
    auto channel = Channel("overflow", 0xffffff);
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, {channel}, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    std::vector<LoggingChannel> channels;
    for (int i = 0; i < 22; ++i)
    {
        channels.push_back(Channel(std::to_string(i), 0x1000));
        channels.back().length = 4;
    }
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, channels, ValidPolicy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    channels.pop_back();
    EXPECT_THAT(MakeLoggingSession(LoggingProtocolId::kSsm, channels, ValidPolicy()), fastecu::testing::IsOk());
}
