#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logging_conversion.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/error.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "",
        .decimal_precision = 15,
    };
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

LoggingSession make_session_with_channel(LoggingChannel channel)
{
    auto session = make_logging_session(LoggingProtocolId::Ssm, {std::move(channel)}, valid_policy());
    EXPECT_THAT(session, fastecu::testing::IsOk());
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
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->raw_value, "1616");
    EXPECT_DOUBLE_EQ(result->numeric_value, 404.0);
    EXPECT_EQ(result->unit, "rpm");
}

TEST(LoggingConversionTest, RejectsUnknownOrMismatchedChannel)
{
    auto session = make_valid_session();
    ASSERT_THAT(convert_sample(session, ProtocolSample{"missing", "12"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::Internal));
}

TEST(LoggingConversionTest, PreservesProtocolRawValueWithoutReassembly)
{
    auto session = make_valid_session();
    auto result = convert_sample(session, ProtocolSample{"rpm", "0012"});
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->raw_value, "0012");
    EXPECT_DOUBLE_EQ(result->numeric_value, 12.0);
}

TEST(LoggingConversionTest, RejectsNonFiniteConvertedValues)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression = "x/(x-1)";
    auto session = make_session_with_channel(c);

    ASSERT_THAT(convert_sample(session, ProtocolSample{"rpm", "1"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(LoggingConversionTest, UsesHistoricalFifteenDigitIntermediatePrecision)
{
    auto c = channel("rpm", 0x10);
    c.from_byte_expression = "x/3*3";
    c.decimal_precision = 2;
    auto session = make_session_with_channel(c);

    auto result = convert_sample(session, ProtocolSample{"rpm", "10"});

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_DOUBLE_EQ(result->numeric_value, 9.9999999999999893);
    EXPECT_NE(result->numeric_value, 9.9);
}

TEST(LoggingConversionTest, ExtractsSwitchBitInsteadOfTreatingWholeByteAsValue)
{
    auto source = channel("switch:flag", 0x20);
    source.length = 1;
    source.decimal_precision = 0;
    source.sample_bit = 7;
    const auto session = make_session_with_channel(source);
    const auto on = convert_sample(session, {"switch:flag", "128"});
    const auto off = convert_sample(session, {"switch:flag", "0"});
    ASSERT_THAT(on, fastecu::testing::IsOk());
    ASSERT_THAT(off, fastecu::testing::IsOk());
    EXPECT_EQ(on->numeric_value, 1);
    EXPECT_EQ(off->numeric_value, 0);
    EXPECT_THAT(convert_sample(session, {"switch:flag", "256"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
}
