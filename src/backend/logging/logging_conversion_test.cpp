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

fastecu::Result<LoggingSession> MakeSessionWithChannel(LoggingChannel channel)
{
    return MakeLoggingSession(LoggingProtocolId::kSsm, {std::move(channel)}, ValidPolicy());
}

fastecu::Result<LoggingSession> MakeValidSession()
{
    return MakeSessionWithChannel(Channel("rpm", 0x10));
}

} // namespace

TEST(LoggingConversionTest, ScalesDecodedSsmNumericInput)
{
    LoggingChannel c = Channel("rpm", 0x10);
    c.from_byte_expression = "x/4";
    c.unit = "rpm";
    auto built_session_1 = MakeSessionWithChannel(c);
    ASSERT_THAT(built_session_1, fastecu::testing::IsOk());
    auto session = std::move(*built_session_1);
    auto result = ConvertSample(session, ProtocolSample{"rpm", "258"});
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->raw_value, "258");
    EXPECT_EQ(result->numeric_value, 64.5);
    EXPECT_EQ(result->unit, "rpm");
}

TEST(LoggingConversionTest, RejectsUnknownOrMismatchedChannel)
{
    auto built_session_2 = MakeValidSession();
    ASSERT_THAT(built_session_2, fastecu::testing::IsOk());
    auto session = std::move(*built_session_2);
    ASSERT_THAT(ConvertSample(session, ProtocolSample{"missing", "12"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
}

TEST(LoggingConversionTest, PreservesProtocolRawValueWithoutReassembly)
{
    auto built_session_3 = MakeValidSession();
    ASSERT_THAT(built_session_3, fastecu::testing::IsOk());
    auto session = std::move(*built_session_3);
    auto result = ConvertSample(session, ProtocolSample{"rpm", "0012"});
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->raw_value, "0012");
    EXPECT_EQ(result->numeric_value, 12.0);
}

TEST(LoggingConversionTest, RejectsNonFiniteConvertedValues)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression = "x/(x-1)";
    auto built_session_4 = MakeSessionWithChannel(c);
    ASSERT_THAT(built_session_4, fastecu::testing::IsOk());
    auto session = std::move(*built_session_4);

    ASSERT_THAT(
        ConvertSample(session, ProtocolSample{"rpm", "1"}),
        fastecu::testing::IsErrWith(fastecu::ErrorKind::kInvalidConfig,
                                    ::testing::Eq("logging conversion expression failed: cannot divide by zero")));
}

TEST(LoggingConversionTest, EvaluatesIntermediateResultsWithoutTextRounding)
{
    auto c = Channel("rpm", 0x10);
    c.from_byte_expression = "x/3*3";
    c.decimal_precision = 2;
    auto built_session_5 = MakeSessionWithChannel(c);
    ASSERT_THAT(built_session_5, fastecu::testing::IsOk());
    auto session = std::move(*built_session_5);

    auto result = ConvertSample(session, ProtocolSample{"rpm", "10"});

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->numeric_value, 10.0);
}

TEST(LoggingConversionTest, RejectsNonNumericRawValue)
{
    auto built_session_6 = MakeValidSession();
    ASSERT_THAT(built_session_6, fastecu::testing::IsOk());
    auto session = std::move(*built_session_6);

    ASSERT_THAT(ConvertSample(session, ProtocolSample{"rpm", "12ab"}),
                fastecu::testing::IsErrWith(
                    fastecu::ErrorKind::kBadResponse,
                    ::testing::Eq("protocol sample raw value is not a finite number: invalid numeric literal")));
}

TEST(LoggingConversionTest, PreparesExpressionWhoseDomainExcludesFormerProbes)
{
    auto channel = Channel("rpm", 0x10);
    channel.from_byte_expression = "1/((x-1)*(x-16)*(x-1616))";
    auto session = MakeLoggingSession(LoggingProtocolId::kSsm, {channel}, ValidPolicy());
    ASSERT_THAT(session, fastecu::testing::IsOk());
    ASSERT_THAT(ConvertSample(*session, ProtocolSample{"rpm", "2"}), fastecu::testing::IsOk());
    EXPECT_THAT(ConvertSample(*session, ProtocolSample{"rpm", "16"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(LoggingConversionTest, ExtractsSwitchBitInsteadOfTreatingWholeByteAsValue)
{
    auto source = Channel("switch:flag", 0x20);
    source.length = 1;
    source.decimal_precision = 0;
    source.sample_bit = 7;
    auto built_session_7 = MakeSessionWithChannel(source);
    ASSERT_THAT(built_session_7, fastecu::testing::IsOk());
    const auto session = std::move(*built_session_7);
    const auto on = ConvertSample(session, {"switch:flag", "128"});
    const auto off = ConvertSample(session, {"switch:flag", "0"});
    ASSERT_THAT(on, fastecu::testing::IsOk());
    ASSERT_THAT(off, fastecu::testing::IsOk());
    EXPECT_EQ(on->numeric_value, 1);
    EXPECT_EQ(off->numeric_value, 0);
    EXPECT_THAT(ConvertSample(session, {"switch:flag", "256"}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}
