#include "src/backend/logging/logging_channel_preparation.h"

#include <array>
#include <format>
#include "src/backend/logging/logger_definition_parser.h"
#include <cstdint>
#include <string>
#include <utility>

#include <gmock/gmock-matchers.h>
#include <gmock/gmock.h>
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
using ::testing::IsEmpty;

LoggerParameter Parameter()
{
    return {.protocol = "SSM",
            .id = "rpm",
            .address = "000010",
            .length = "1",
            .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}};
}

TEST(LoggingChannelPreparation, PreservesExplicitAddressOrder)
{
    auto p = Parameter();
    p.address = "0x11";
    p.address_specs = {{.value = "0x11"}, {.value = "0x10"}};
    const auto result = PrepareLoggingChannel(p, LoggingProtocolId::kSsm);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->length, 2U);
    EXPECT_EQ(result->byte_addresses, (std::vector<std::uint32_t>{0x11, 0x10}));
}
TEST(LoggingChannelPreparation, CanonicalXmlWidthIgnoresParentAndCompilesDomainRestrictedExpression)
{
    const std::string xml = R"xml(<logger><protocols><protocol id="SSM"><parameters>
      <parameter id="rpm" length="4"><address length="2">0x10</address><conversions>
      <conversion expr="1/((x-1)*(x-16)*(x-1616))" format="0"/>
      </conversions></parameter></parameters></protocol></protocols></logger>)xml";
    const auto definition =
        ParseLoggerDefinition({reinterpret_cast<const bytes::Byte *>(xml.data()), xml.size()}, "canonical.xml");
    ASSERT_THAT(definition, IsOk());
    const auto channel = PrepareLoggingChannel(definition->parameters.front(), LoggingProtocolId::kSsm);
    ASSERT_THAT(channel, IsOk());
    EXPECT_EQ(channel->length, 2U);
}

TEST(LoggingChannelPreparation, NestedSwitchAddressDoesNotSupplyMissingCanonicalSource)
{
    const std::string xml = R"xml(<logger><protocols><protocol id="SSM"><switches>
      <switch id="flag"><address bit="5">20</address></switch>
      </switches></protocol></protocols></logger>)xml";
    const auto definition =
        ParseLoggerDefinition({reinterpret_cast<const bytes::Byte *>(xml.data()), xml.size()}, "switch.xml");
    ASSERT_THAT(definition, IsOk());
    EXPECT_THAT(PrepareLoggingSwitch(definition->switches.front(), LoggingProtocolId::kSsm),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("flag")));
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
    auto p = Parameter();
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
    EXPECT_THAT(PrepareLoggingChannel(p, LoggingProtocolId::kSsm),
                IsErrWith(ErrorKind::kInvalidConfig,
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
                      InvalidInput{"format", " 0.00 "}, InvalidInput{"format", "0." + std::string(16, '0')}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{
                "EmptyAddress",    "MissingHexAddress", "PositiveSignedAddress", "NegativeAddress",
                "AddressJunk",     "AddressWhitespace", "AddressOverflow",       "AddressUnicodeWhitespace",
                "EmptyLength",     "ZeroLength",        "PositiveSignedLength",  "NegativeLength",
                "LengthJunk",      "HexLength",         "LengthWhitespace",      "LengthOverflow",
                "EmptyFormat",     "UnknownFormat",     "ZeroPrecision",         "FormatJunk",
                "MultipleFormats", "NullFormat",        "StringFormat"};
            return kNames[param_info.index];
        }));

TEST(LoggingChannelPreparation, AcceptsHexadecimalAddressesAndAsciiWhitespace)
{
    for (const auto& address : {"000010", "0x10", "0X10", " \t0x10\r\n"})
    {
        SCOPED_TRACE(address);
        auto p = Parameter();
        p.address = address;
        const auto result = PrepareLoggingChannel(p, LoggingProtocolId::kSsm);
        ASSERT_THAT(result, IsOk());
        EXPECT_EQ(result->address, 0x10U);
    }
}

TEST(LoggingChannelPreparation, AcceptsDecimalLengths)
{
    for (const auto& [input, expected] :
         std::array<std::pair<const char *, std::size_t>, 3>{{{"1", 1}, {" 2 ", 2}, {"04", 4}}})
    {
        SCOPED_TRACE(input);
        auto p = Parameter();
        p.length = input;
        const auto result = PrepareLoggingChannel(p, LoggingProtocolId::kSsm);
        ASSERT_THAT(result, IsOk());
        EXPECT_EQ(result->length, expected);
    }
}

TEST(LoggingChannelPreparation, ResolvesFirstConversionAndAllowsEmptyUnits)
{
    auto p = Parameter();
    p.conversions.front().units.clear();
    p.conversions.push_back({"other", "x*2", "0.000", "0", "100", "1"});
    const auto result = PrepareLoggingChannel(p, LoggingProtocolId::kSsm);
    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(result->unit, IsEmpty());
    EXPECT_EQ(result->from_byte_expression, "x");
    EXPECT_EQ(result->decimal_precision, 2);
}

TEST(LoggingChannelPreparation, AcceptsFixedDecimalFormatsThroughFifteenPlaces)
{
    for (const auto precision : {0U, 1U, 2U, 15U})
    {
        SCOPED_TRACE(precision);
        auto p = Parameter();
        p.conversions.front().format = precision == 0 ? "0" : "0." + std::string(precision, '0');
        const auto result = PrepareLoggingChannel(p, LoggingProtocolId::kSsm);
        ASSERT_THAT(result, IsOk());
        EXPECT_EQ(result->decimal_precision, precision);
    }
}

TEST(LoggingChannelPreparation, RejectsMissingConversionAndInvalidExpressions)
{
    auto p = Parameter();
    p.conversions.clear();
    EXPECT_THAT(PrepareLoggingChannel(p, LoggingProtocolId::kSsm),
                IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("conversion"))));
    for (const auto expression : {"", "x+", "1/0"})
    {
        SCOPED_TRACE(expression);
        p = Parameter();
        p.conversions.front().expr = expression;
        EXPECT_THAT(PrepareLoggingChannel(p, LoggingProtocolId::kSsm),
                    IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("expression"))));
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
    for (const auto& item : std::array<Case, 5>{{{LoggingProtocolId::kSsm, "ffffff", true},
                                                 {LoggingProtocolId::kSsm, "1000000", false},
                                                 {LoggingProtocolId::kMutDma, "ffff", true},
                                                 {LoggingProtocolId::kMutDma, "10000", false},
                                                 {LoggingProtocolId::kCdbg, "ffffffff", true}}})
    {
        SCOPED_TRACE(item.address);
        auto p = Parameter();
        p.address = item.address;
        const auto result = PrepareLoggingChannel(p, item.protocol);
        if (item.valid)
        {
            ASSERT_THAT(result, IsOk());
        }
        else
        {
            EXPECT_THAT(result, IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("address")));
        }
    }
}

TEST(LoggingChannelPreparation, RetainsProtocolLengthBounds)
{
    for (const auto protocol : {LoggingProtocolId::kSsm, LoggingProtocolId::kMutDma, LoggingProtocolId::kCdbg})
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        for (const auto length : {1U, 2U, 3U, 4U, 255U, 256U})
        {
            SCOPED_TRACE(static_cast<int>(protocol));
            SCOPED_TRACE(length);
            auto p = Parameter();
            p.length = std::to_string(length);
            const auto result = PrepareLoggingChannel(p, protocol);
            const bool valid = length == 1 || length == 2 || length == 4;
            if (valid)
            {
                ASSERT_THAT(result, IsOk());
            }
            else
            {
                EXPECT_THAT(result, IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("length")));
            }
        }
    }
}
TEST(LoggingChannelPreparation, SyntaxErrorIncludesSourcePosition)
{
    auto parameter = Parameter();
    parameter.conversions.front().expr = "x+";
    EXPECT_THAT(PrepareLoggingChannel(parameter, LoggingProtocolId::kSsm),
                IsErrWith(ErrorKind::kInvalidConfig, AllOf(HasSubstr("rpm"), HasSubstr("position"))));
}

TEST(LoggingChannelPreparation, RetainsSharedEvaluatorSyntax)
{
    auto parameter = Parameter();
    parameter.conversions.front().expr = "+(x*1e2)";
    EXPECT_THAT(PrepareLoggingChannel(parameter, LoggingProtocolId::kSsm), IsOk());
}

TEST(LoggingChannelPreparation, RejectsInvalidSelectedNumericMetadata)
{
    for (const auto fields : {R"(storagetype="bogus")", R"(storagetype="float")", R"(endian="middle")"})
    {
        SCOPED_TRACE(fields);
        const auto xml = std::format(R"(<logger><protocols><protocol id="SSM"><parameters>
          <parameter id="rpm"><address length="2">0x1000</address><conversions>
          <conversion {} expr="x" format="0"/></conversions></parameter>
          </parameters></protocol></protocols></logger>)",
                                     fields);
        const auto definition =
            ParseLoggerDefinition({reinterpret_cast<const bytes::Byte *>(xml.data()), xml.size()}, "metadata fixture");
        ASSERT_THAT(definition, IsOk());
        EXPECT_THAT(PrepareLoggingChannel(definition->parameters.front(), LoggingProtocolId::kSsm),
                    IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("rpm")));
    }
}

TEST(LoggingChannelPreparation, RejectsUnsupportedNondefaultNumericMetadataForOtherProtocols)
{
    for (const auto protocol : {LoggingProtocolId::kMutDma, LoggingProtocolId::kCdbg})
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        auto parameter = Parameter();
        parameter.protocol = protocol == LoggingProtocolId::kMutDma ? "MUT_DMA" : "CDBG";
        parameter.conversions.front().storage_type = "int16";
        EXPECT_THAT(PrepareLoggingChannel(parameter, protocol),
                    IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("storagetype")));
        parameter.conversions.front().storage_type.clear();
        parameter.conversions.front().endian = "little";
        EXPECT_THAT(PrepareLoggingChannel(parameter, protocol),
                    IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("endian")));
        parameter.conversions.front().endian = "big";
        EXPECT_THAT(PrepareLoggingChannel(parameter, protocol), IsOk());
    }
}
} // namespace
} // namespace fastecu::logging
