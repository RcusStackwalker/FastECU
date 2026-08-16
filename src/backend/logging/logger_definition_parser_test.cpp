#include "src/backend/logging/logger_definition_parser.h"

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::logging::parse_logger_definition;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::SizeIs;

bytes::ByteView view(std::string_view text)
{
    return {reinterpret_cast<const bytes::Byte *>(text.data()), text.size()};
}

constexpr std::string_view kWellFormed = R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" name="Engine Speed" desc="RPM" length="2"
             ecubyteindex="3" ecubit="4" target="1" enabled="1">
    <address>0x1234</address>
    <conversions>
      <conversion units="rpm" expr="x*0.25" format="0.00"
                  gauge_min="0" gauge_max="8000" gauge_step="500"/>
      <conversion units="rps" expr="x/240" format="0.0"
                  gauge_min="0" gauge_max="133" gauge_step="10"/>
    </conversions>
  </parameter>
</parameters><switches>
  <switch id="S1" name="Test Switch" desc="flag" byte="0x20"
          ecubyteindex="7" bit="1" target="2"/>
</switches></protocol></protocols></logger>)";

TEST(LoggerDefinitionParser, ParsesParametersSwitchesAndConversions)
{
    const auto result = parse_logger_definition(view(kWellFormed), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_THAT(result->parameters, SizeIs(1));
    const auto& p = result->parameters.at(0);
    EXPECT_EQ(p.protocol, "SSM");
    EXPECT_EQ(p.id, "P1");
    EXPECT_EQ(p.name, "Engine Speed");
    EXPECT_EQ(p.description, "RPM");
    EXPECT_EQ(p.address, "0x1234");
    EXPECT_EQ(p.length, "2");
    EXPECT_EQ(p.ecu_byte_index, "3");
    EXPECT_EQ(p.ecu_bit, "4");
    EXPECT_EQ(p.target, "1");
    EXPECT_TRUE(p.enabled);

    ASSERT_THAT(p.conversions, SizeIs(2));
    EXPECT_EQ(p.conversions.at(0).units, "rpm");
    EXPECT_EQ(p.conversions.at(0).expr, "x*0.25");
    EXPECT_EQ(p.conversions.at(0).format, "0.00");
    EXPECT_EQ(p.conversions.at(0).gauge_min, "0");
    EXPECT_EQ(p.conversions.at(0).gauge_max, "8000");
    EXPECT_EQ(p.conversions.at(0).gauge_step, "500");
    EXPECT_EQ(p.conversions.at(1).units, "rps");
    EXPECT_EQ(p.conversions.at(1).expr, "x/240");

    ASSERT_THAT(result->switches, SizeIs(1));
    const auto& s = result->switches.at(0);
    EXPECT_EQ(s.protocol, "SSM");
    EXPECT_EQ(s.id, "S1");
    EXPECT_EQ(s.name, "Test Switch");
    EXPECT_EQ(s.description, "flag");
    EXPECT_EQ(s.address, "0x20");
    EXPECT_EQ(s.ecu_byte_index, "7");
    EXPECT_EQ(s.ecu_bit, "1");
    EXPECT_EQ(s.target, "2");
    // The definition XML carries no switch enabled attribute; always false
    // out of the parser (file_actions.cpp:1261).
    EXPECT_FALSE(s.enabled);
}

TEST(LoggerDefinitionParser, SubstitutesLegacyPlaceholderDefaults)
{
    constexpr std::string_view kBare =
        R"(<logger><protocols><protocol><parameters>
  <parameter><address>0x1</address>
    <conversions><conversion/></conversions>
  </parameter>
</parameters><switches><switch/></switches></protocol></protocols></logger>)";

    const auto result = parse_logger_definition(view(kBare), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& p = result->parameters.at(0);
    EXPECT_EQ(p.protocol, "No protocol id");
    EXPECT_EQ(p.id, "No id");
    EXPECT_EQ(p.name, "No name");
    EXPECT_EQ(p.description, "No desc");
    EXPECT_EQ(p.ecu_byte_index, "No byte index");
    EXPECT_EQ(p.ecu_bit, "No ecu bit");
    EXPECT_EQ(p.target, "No target");
    EXPECT_EQ(p.length, "1");
    EXPECT_FALSE(p.enabled);

    const auto& c = p.conversions.at(0);
    EXPECT_EQ(c.units, "#");
    EXPECT_EQ(c.expr, "x");
    EXPECT_EQ(c.format, "0.00");
    EXPECT_EQ(c.gauge_min, "No gauge_min");
    EXPECT_EQ(c.gauge_max, "No gauge_max");
    EXPECT_EQ(c.gauge_step, "No gauge_step");

    const auto& s = result->switches.at(0);
    EXPECT_EQ(s.id, "No id");
    EXPECT_EQ(s.address, "No address");
    EXPECT_EQ(s.ecu_byte_index, "No ecu byte index");
    EXPECT_EQ(s.ecu_bit, "No ecu bit");
    EXPECT_FALSE(s.enabled);
}

// The fix ruled on in "Decisions locked": rows stay aligned even when a
// <parameter> omits <address> or <conversions>. The legacy parser skewed the
// parallel arrays here; see file_actions_parsing_test's characterization.
TEST(LoggerDefinitionParser, RowsStayAlignedWhenOptionalChildrenAreMissing)
{
    constexpr std::string_view kSparse =
        R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1"><address>0x10</address></parameter>
  <parameter id="P2">
    <conversions><conversion units="rpm"/></conversions>
  </parameter>
  <parameter id="P3"/>
</parameters></protocol></protocols></logger>)";

    const auto result = parse_logger_definition(view(kSparse), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_THAT(result->parameters, SizeIs(3));

    EXPECT_EQ(result->parameters.at(0).address, "0x10");
    EXPECT_THAT(result->parameters.at(0).conversions, IsEmpty());
    EXPECT_EQ(result->parameters.at(1).address, "");
    EXPECT_THAT(result->parameters.at(1).conversions, SizeIs(1));
    EXPECT_EQ(result->parameters.at(2).address, "");
    EXPECT_THAT(result->parameters.at(2).conversions, IsEmpty());
}

TEST(LoggerDefinitionParser, CollectsParametersAcrossMultipleProtocols)
{
    constexpr std::string_view kTwo =
        R"(<logger><protocols>
  <protocol id="SSM"><parameters><parameter id="P1"/></parameters></protocol>
  <protocol id="CDBG"><parameters><parameter id="P2"/></parameters></protocol>
</protocols></logger>)";

    const auto result = parse_logger_definition(view(kTwo), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_THAT(result->parameters, ElementsAre(Field(&fastecu::logging::LoggerParameter::protocol, "SSM"),
                                                Field(&fastecu::logging::LoggerParameter::protocol, "CDBG")));
}

TEST(LoggerDefinitionParser, RejectsMalformedXml)
{
    const auto result = parse_logger_definition(view("<logger><protocols>"), "broken.xml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("broken.xml"));
}

TEST(LoggerDefinitionParser, RejectsWrongRootElement)
{
    const auto result = parse_logger_definition(view("<config/>"), "wrong.xml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggerDefinitionParser, AcceptsAnEmptyButWellFormedDocument)
{
    const auto result = parse_logger_definition(view("<logger/>"), "empty.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_THAT(result->parameters, IsEmpty());
    EXPECT_THAT(result->switches, IsEmpty());
}

} // namespace
