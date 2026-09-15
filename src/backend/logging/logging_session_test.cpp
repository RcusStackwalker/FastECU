#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logging_session.h"

#include <chrono>

#include <gtest/gtest.h>

namespace
{

using namespace fastecu::logging;
using namespace std::chrono_literals;

LoggingChannel channel(std::string id, std::uint32_t address)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = 2,
        .raw_assembly = RawAssembly::DecimalBytesConcatenated,
        .from_byte_expression = "x",
        .unit = "rpm",
        .decimal_precision = 2,
    };
}

std::vector<LoggingChannel> valid_channels()
{
    return {channel("rpm", 0x10)};
}

LoggingPolicy valid_policy()
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
    auto channels = valid_channels();
    channels.push_back(channels.front());
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, channels, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingSessionTest, StableIdsSurviveSourceRowReordering)
{
    auto result =
        make_logging_session(LoggingProtocolId::Ssm, {channel("rpm", 0x10), channel("coolant", 0x20)}, valid_policy());
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->channels()[0].id, "rpm");
    EXPECT_EQ(result->channels()[1].id, "coolant");
    EXPECT_EQ(result->find_channel("coolant")->address, 0x20U);
}

TEST(LoggingSessionTest, RejectsInvalidPolicy)
{
    auto policy = valid_policy();
    policy.poll_timeout = 0ms;
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, valid_channels(), policy),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingSessionTest, RejectsInvalidChannelShape)
{
    auto c = channel("rpm", 0x10);
    c.length = 0;
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingSessionTest, RejectsInvalidChannelIdentityAndAssembly)
{
    auto c = channel("", 0x10);
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));

    c = channel("rpm", 0x10);
    c.length = 256;
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));

    c = channel("rpm", 0x10);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- exercises the invalid-value rejection path
    c.raw_assembly = static_cast<RawAssembly>(99);
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingSessionTest, RejectsOutOfRangeProtocolAddresses)
{
    auto c = channel("rpm", 0x1000000);
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.address = 0x10000;
    ASSERT_THAT(make_logging_session(LoggingProtocolId::MutDma, {c}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.address = 0xffffffff;
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Cdbg, {c}, valid_policy()), fastecu::testing::IsOk());
}

TEST(LoggingSessionTest, RejectsInvalidConversionConfiguration)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression.clear();
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.from_byte_expression = "x+invalid";
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    c.from_byte_expression = "x";
    c.decimal_precision = 16;
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));
}

TEST(LoggingSessionTest, RejectsExpressionsWithoutFiniteEvaluation)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression = "x/0";
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));

    c.from_byte_expression = "0/0";
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingSessionTest, RequiresAtLeastOneCdbgChannel)
{
    ASSERT_THAT(make_logging_session(LoggingProtocolId::Cdbg, {}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));

    EXPECT_THAT(make_logging_session(LoggingProtocolId::Ssm, {}, valid_policy()), fastecu::testing::IsOk());

    EXPECT_THAT(make_logging_session(LoggingProtocolId::MutDma, {}, valid_policy()), fastecu::testing::IsOk());
}

TEST(LoggingSessionTest, RejectsUnknownProtocolIdentifiers)
{
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- exercising the invalid-value rejection path
    ASSERT_THAT(make_logging_session(static_cast<LoggingProtocolId>(99), {}, valid_policy()),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingSessionTest, RejectsProtocolSpecificWireShapesBeforeIo)
{
    auto ssm_channel = channel("ssm", 0x10);
    ssm_channel.length = 256;
    EXPECT_THAT(make_logging_session(LoggingProtocolId::Ssm, {ssm_channel}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    auto mut_channel = channel("mut", 0x10);
    mut_channel.length = 3;
    EXPECT_THAT(make_logging_session(LoggingProtocolId::MutDma, {mut_channel}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    auto cdbg_channel = channel("cdbg", 0x804000);
    cdbg_channel.length = 3;
    EXPECT_THAT(make_logging_session(LoggingProtocolId::Cdbg, {cdbg_channel}, valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    std::vector<LoggingChannel> too_many_cdbg_channels;
    for (int i = 0; i < 57; ++i)
    {
        too_many_cdbg_channels.push_back(channel("cdbg-" + std::to_string(i), 0x804000 + i));
        too_many_cdbg_channels.back().length = 1;
    }
    EXPECT_THAT(make_logging_session(LoggingProtocolId::Cdbg, std::move(too_many_cdbg_channels), valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    std::vector<LoggingChannel> too_many_ssm_channels;
    for (int i = 0; i < 85; ++i)
    {
        too_many_ssm_channels.push_back(channel("ssm-" + std::to_string(i), i));
        too_many_ssm_channels.back().length = 1;
    }
    EXPECT_THAT(make_logging_session(LoggingProtocolId::Ssm, std::move(too_many_ssm_channels), valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));

    std::vector<LoggingChannel> too_many_mut_channels;
    for (int i = 0; i < 256; ++i)
    {
        too_many_mut_channels.push_back(channel("mut-" + std::to_string(i), i));
        too_many_mut_channels.back().length = 1;
    }
    EXPECT_THAT(make_logging_session(LoggingProtocolId::MutDma, std::move(too_many_mut_channels), valid_policy()),
                ::testing::Not(fastecu::testing::IsOk()));
}
