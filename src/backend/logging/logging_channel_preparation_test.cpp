#include "src/backend/logging/logging_channel_preparation.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::logging
{
namespace
{
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::AllOf;
using ::testing::HasSubstr;

LoggerParameter parameter()
{
    return {.protocol = "SSM",
            .id = "rpm",
            .address = "000010",
            .length = "1",
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}

struct InvalidInput
{
    std::string field;
    std::string input;
};

class LoggingChannelPreparationInvalidInput : public ::testing::TestWithParam<InvalidInput>
{
};

TEST_P(LoggingChannelPreparationInvalidInput, RejectsWithAuthorContext)
{
    const auto& [field, input] = GetParam();
    auto p = parameter();
    if (field == "address")
    {
        p.address = input;
    }
    else if (field == "length")
    {
        p.length = input;
    }
    else
    {
        p.conversions.front().format = input;
    }
    EXPECT_THAT(prepare_logging_channel(p, LoggingProtocolId::Ssm),
                IsErrWith(ErrorKind::InvalidConfig,
                          AllOf(HasSubstr("SSM"), HasSubstr("rpm"), HasSubstr(field), HasSubstr(input))));
}

INSTANTIATE_TEST_SUITE_P(
    DefinitionSyntax, LoggingChannelPreparationInvalidInput,
    ::testing::Values(InvalidInput{"address", ""}, InvalidInput{"address", "0x"}, InvalidInput{"address", "+10"},
                      InvalidInput{"address", "-10"}, InvalidInput{"address", "10junk"}, InvalidInput{"address", "1 0"},
                      InvalidInput{"address", "100000000"},
                      InvalidInput{"address", "\xc2\xa0"
                                              "10"
                                              "\xc2\xa0"},
                      InvalidInput{"length", ""}, InvalidInput{"length", "0"}, InvalidInput{"length", "+1"},
                      InvalidInput{"length", "-1"}, InvalidInput{"length", "1x"}, InvalidInput{"length", "0x2"},
                      InvalidInput{"length", "1 0"}, InvalidInput{"length", "18446744073709551616"},
                      InvalidInput{"format", ""}, InvalidInput{"format", "banana"}, InvalidInput{"format", "0."},
                      InvalidInput{"format", "0.0garbage0"}, InvalidInput{"format", "0.00.000"},
                      InvalidInput{"format", " 0.00 "}, InvalidInput{"format", "0." + std::string(16, '0')}));

TEST(LoggingChannelPreparation, AcceptsHexadecimalAddressesAndAsciiWhitespace)
{
    for (const auto& address : {"000010", "0x10", "0X10", " \t0x10\r\n"})
    {
        SCOPED_TRACE(address);
        auto p = parameter();
        p.address = address;
        const auto result = prepare_logging_channel(p, LoggingProtocolId::Ssm);
        ASSERT_THAT(result, IsOk());
        EXPECT_EQ(result->address, 0x10U);
        EXPECT_EQ(result->raw_assembly, RawAssembly::DecimalBytesConcatenated);
    }
}

TEST(LoggingChannelPreparation, AcceptsDecimalLengths)
{
    for (const auto& [input, expected] :
         std::array<std::pair<const char *, std::size_t>, 3>{{{"1", 1}, {" 2 ", 2}, {"04", 4}}})
    {
        SCOPED_TRACE(input);
        auto p = parameter();
        p.length = input;
        const auto result = prepare_logging_channel(p, LoggingProtocolId::Ssm);
        ASSERT_THAT(result, IsOk());
        EXPECT_EQ(result->length, expected);
    }
}

TEST(LoggingChannelPreparation, ResolvesFirstConversionAndAllowsEmptyUnits)
{
    auto p = parameter();
    p.conversions.front().units.clear();
    p.conversions.push_back({"other", "x*2", "0.000", "0", "100", "1"});
    const auto result = prepare_logging_channel(p, LoggingProtocolId::Ssm);
    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(result->unit.empty());
    EXPECT_EQ(result->from_byte_expression, "x");
    EXPECT_EQ(result->decimal_precision, 2);
}

TEST(LoggingChannelPreparation, AcceptsFixedDecimalFormatsThroughFifteenPlaces)
{
    for (const auto precision : {0U, 1U, 2U, 15U})
    {
        SCOPED_TRACE(precision);
        auto p = parameter();
        p.conversions.front().format = precision == 0 ? "0" : "0." + std::string(precision, '0');
        const auto result = prepare_logging_channel(p, LoggingProtocolId::Ssm);
        ASSERT_THAT(result, IsOk());
        EXPECT_EQ(result->decimal_precision, precision);
    }
}

TEST(LoggingChannelPreparation, RejectsMissingConversionAndInvalidExpressions)
{
    auto p = parameter();
    p.conversions.clear();
    EXPECT_THAT(prepare_logging_channel(p, LoggingProtocolId::Ssm),
                IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("conversion"))));
    for (const auto expression : {"", "x+", "1/0"})
    {
        SCOPED_TRACE(expression);
        p = parameter();
        p.conversions.front().expr = expression;
        EXPECT_THAT(prepare_logging_channel(p, LoggingProtocolId::Ssm),
                    IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("expression"))));
    }
}

TEST(LoggingChannelPreparation, RetainsProtocolAddressBounds)
{
    struct Case
    {
        LoggingProtocolId protocol;
        const char *address;
        bool valid;
    };
    for (const auto& item : std::array<Case, 5>{{{LoggingProtocolId::Ssm, "ffffff", true},
                                                 {LoggingProtocolId::Ssm, "1000000", false},
                                                 {LoggingProtocolId::MutDma, "ffff", true},
                                                 {LoggingProtocolId::MutDma, "10000", false},
                                                 {LoggingProtocolId::Cdbg, "ffffffff", true}}})
    {
        SCOPED_TRACE(item.address);
        auto p = parameter();
        p.address = item.address;
        const auto result = prepare_logging_channel(p, item.protocol);
        if (item.valid)
        {
            ASSERT_THAT(result, IsOk());
        }
        else
        {
            EXPECT_THAT(result, IsErrWith(ErrorKind::InvalidConfig, HasSubstr("address")));
        }
    }
}

TEST(LoggingChannelPreparation, RetainsProtocolLengthBounds)
{
    for (const auto protocol : {LoggingProtocolId::Ssm, LoggingProtocolId::MutDma, LoggingProtocolId::Cdbg})
    {
        for (const auto length : {1U, 2U, 3U, 4U, 255U, 256U})
        {
            SCOPED_TRACE(static_cast<int>(protocol));
            SCOPED_TRACE(length);
            auto p = parameter();
            p.length = std::to_string(length);
            const auto result = prepare_logging_channel(p, protocol);
            const bool valid =
                protocol == LoggingProtocolId::Ssm ? length <= 255 : length == 1 || length == 2 || length == 4;
            if (valid)
            {
                ASSERT_THAT(result, IsOk());
            }
            else
            {
                EXPECT_THAT(result, IsErrWith(ErrorKind::InvalidConfig, HasSubstr("length")));
            }
        }
    }
}
} // namespace
} // namespace fastecu::logging
