#include "src/backend/logging/logging_session.h"

#include <gtest/gtest.h>

namespace
{

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
        .poll_timeout_ms = 100,
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
    auto result = make_logging_session(LoggingProtocolId::Ssm, channels, valid_policy());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggingSessionTest, StableIdsSurviveSourceRowReordering)
{
    auto result = make_logging_session(
        LoggingProtocolId::Ssm,
        {channel("rpm", 0x10), channel("coolant", 0x20)}, valid_policy());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->channels()[0].id, "rpm");
    EXPECT_EQ(result->channels()[1].id, "coolant");
    EXPECT_EQ(result->find_channel("coolant")->address, 0x20u);
}

TEST(LoggingSessionTest, RejectsInvalidPolicy)
{
    auto policy = valid_policy();
    policy.poll_timeout_ms = 0;
    auto result = make_logging_session(LoggingProtocolId::Ssm, valid_channels(), policy);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggingSessionTest, RejectsInvalidChannelShape)
{
    auto c = channel("rpm", 0x10);
    c.length = 0;
    auto result = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggingSessionTest, RejectsInvalidChannelIdentityAndAssembly)
{
    auto c = channel("", 0x10);
    auto empty_id = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(empty_id);
    EXPECT_EQ(empty_id.error().kind, fastecu::ErrorKind::InvalidConfig);

    c = channel("rpm", 0x10);
    c.length = 256;
    auto excessive_length = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(excessive_length);
    EXPECT_EQ(excessive_length.error().kind, fastecu::ErrorKind::InvalidConfig);

    c = channel("rpm", 0x10);
    c.raw_assembly = static_cast<RawAssembly>(99);
    auto invalid_assembly = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(invalid_assembly);
    EXPECT_EQ(invalid_assembly.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggingSessionTest, RejectsOutOfRangeProtocolAddresses)
{
    auto c = channel("rpm", 0x1000000);
    auto ssm = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(ssm);

    c.address = 0x10000;
    auto mut_dma = make_logging_session(LoggingProtocolId::MutDma, {c}, valid_policy());
    ASSERT_FALSE(mut_dma);

    c.address = 0xffffffff;
    auto cdbg = make_logging_session(LoggingProtocolId::Cdbg, {c}, valid_policy());
    ASSERT_TRUE(cdbg);
}

TEST(LoggingSessionTest, RejectsInvalidConversionConfiguration)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression.clear();
    auto empty_expression = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(empty_expression);

    c.from_byte_expression = "x+invalid";
    auto malformed_expression = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(malformed_expression);

    c.from_byte_expression = "x";
    c.decimal_precision = 16;
    auto excessive_precision = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(excessive_precision);
}

TEST(LoggingSessionTest, RejectsExpressionsWithoutFiniteEvaluation)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression = "x/0";
    auto division_by_zero = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(division_by_zero);
    EXPECT_EQ(division_by_zero.error().kind, fastecu::ErrorKind::InvalidConfig);

    c.from_byte_expression = "0/0";
    auto indeterminate = make_logging_session(LoggingProtocolId::Ssm, {c}, valid_policy());
    ASSERT_FALSE(indeterminate);
    EXPECT_EQ(indeterminate.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggingSessionTest, RequiresAtLeastOneCdbgChannel)
{
    auto cdbg = make_logging_session(LoggingProtocolId::Cdbg, {}, valid_policy());
    ASSERT_FALSE(cdbg);
    EXPECT_EQ(cdbg.error().kind, fastecu::ErrorKind::InvalidConfig);

    auto ssm = make_logging_session(LoggingProtocolId::Ssm, {}, valid_policy());
    EXPECT_TRUE(ssm);

    auto mut_dma = make_logging_session(LoggingProtocolId::MutDma, {}, valid_policy());
    EXPECT_TRUE(mut_dma);
}

TEST(LoggingSessionTest, RejectsUnknownProtocolIdentifiers)
{
    auto result = make_logging_session(static_cast<LoggingProtocolId>(99), {}, valid_policy());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}
