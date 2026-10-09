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

LoggingChannel Channel(std::string id, std::uint32_t address)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = 2,
        .raw_assembly = RawAssembly::kUnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "",
        .decimal_precision = 15,
    };
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

LoggingSession MakeSessionWithChannel(LoggingChannel channel)
{
    auto session = MakeLoggingSession(LoggingProtocolId::kSsm, {std::move(channel)}, ValidPolicy());
    EXPECT_THAT(session, fastecu::testing::IsOk());
    return std::move(*session);
}

LoggingSession MakeValidSession()
{
    return MakeSessionWithChannel(Channel("rpm", 0x10));
}

} // namespace

TEST(LoggingConversionTest, PreservesSsmDecimalByteRawInput)
{
    LoggingChannel c = Channel("rpm", 0x10);
    c.raw_assembly = RawAssembly::kDecimalBytesConcatenated;
    c.from_byte_expression = "x/4";
    c.unit = "rpm";
    auto session = MakeSessionWithChannel(c);
    auto result = ConvertSample(session, ProtocolSample{"rpm", "1616"});
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->raw_value, "1616");
    EXPECT_DOUBLE_EQ(result->numeric_value, 404.0);
    EXPECT_EQ(result->unit, "rpm");
}

TEST(LoggingConversionTest, RejectsUnknownOrMismatchedChannel)
{
    auto session = MakeValidSession();
    ASSERT_THAT(ConvertSample(session, ProtocolSample{"missing", "12"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
}

TEST(LoggingConversionTest, PreservesProtocolRawValueWithoutReassembly)
{
    auto session = MakeValidSession();
    auto result = ConvertSample(session, ProtocolSample{"rpm", "0012"});
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->raw_value, "0012");
    EXPECT_DOUBLE_EQ(result->numeric_value, 12.0);
}

TEST(LoggingConversionTest, RejectsNonFiniteConvertedValues)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression = "x/(x-1)";
    auto session = MakeSessionWithChannel(c);

    ASSERT_THAT(ConvertSample(session, ProtocolSample{"rpm", "1"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingConversionTest, EvaluatesIntermediateResultsWithoutTextRounding)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression = "x/3*3";
    c.decimal_precision = 2;
    auto session = MakeSessionWithChannel(c);

    auto result = ConvertSample(session, ProtocolSample{"rpm", "10"});

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_DOUBLE_EQ(result->numeric_value, 10.0);
}

TEST(LoggingConversionTest, RejectsNonNumericRawValue)
{
    auto session = MakeValidSession();

    ASSERT_THAT(ConvertSample(session, ProtocolSample{"rpm", "12ab"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}
