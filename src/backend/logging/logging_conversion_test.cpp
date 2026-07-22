#include "src/backend/logging/logging_conversion.h"

#include <gtest/gtest.h>

namespace
{

LoggingChannel channel(std::string id, std::uint32_t address)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = 2,
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "",
        .decimal_precision = 15,
    };
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

LoggingSession make_session_with_channel(LoggingChannel channel)
{
    auto session = make_logging_session(LoggingProtocolId::Ssm, {std::move(channel)}, valid_policy());
    EXPECT_TRUE(session);
    return std::move(*session);
}

LoggingSession make_valid_session()
{
    return make_session_with_channel(channel("rpm", 0x10));
}

} // namespace

TEST(LoggingConversionTest, PreservesSsmDecimalByteRawInput)
{
    LoggingChannel c = channel("rpm", 0x10);
    c.raw_assembly = RawAssembly::DecimalBytesConcatenated;
    c.from_byte_expression = "x/4";
    c.unit = "rpm";
    auto session = make_session_with_channel(c);
    auto result = convert_sample(session, ProtocolSample{"rpm", "1616"});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->raw_value, "1616");
    EXPECT_DOUBLE_EQ(result->numeric_value, 404.0);
    EXPECT_EQ(result->unit, "rpm");
}

TEST(LoggingConversionTest, RejectsUnknownOrMismatchedChannel)
{
    auto session = make_valid_session();
    auto result = convert_sample(session, ProtocolSample{"missing", "12"});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
}

TEST(LoggingConversionTest, PreservesProtocolRawValueWithoutReassembly)
{
    auto session = make_valid_session();
    auto result = convert_sample(session, ProtocolSample{"rpm", "0012"});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->raw_value, "0012");
    EXPECT_DOUBLE_EQ(result->numeric_value, 12.0);
}

TEST(LoggingConversionTest, RejectsNonFiniteConvertedValues)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression = "x/(x-1)";
    auto session = make_session_with_channel(c);

    auto result = convert_sample(session, ProtocolSample{"rpm", "1"});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}
