#include "src/backend/definition/romraider_parser.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

std::vector<std::uint8_t> bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

void expect_invalid_with_context(
    const Result<UnresolvedDefinition>& result,
    std::string_view source_context,
    std::string_view xml_context)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find(source_context), std::string::npos);
    EXPECT_NE(result.error().detail.find(xml_context), std::string::npos);
}

TEST(RomRaiderParserTest, IndexesMultipleDefinitionsAndRecordsParentReferences)
{
    const auto xml = bytes(R"xml(
      <roms>
        <rom>
          <romid><xmlid>BASE</xmlid><internalidstring>BASE-ID</internalidstring></romid>
        </rom>
        <rom base="BASE">
          <romid>
            <xmlid>CHILD</xmlid><internalidaddress>1A0</internalidaddress>
            <internalidstring>CHILD-ID</internalidstring><ecuid>ECU-1</ecuid>
          </romid>
        </rom>
      </roms>)xml");

    auto result = parse_romraider_index(xml, "rr.xml");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(result->at(0).definition_id, "BASE");
    EXPECT_EQ(result->at(0).internal_id, "BASE-ID");
    EXPECT_EQ(result->at(0).internal_id_encoding, IdEncoding::Ascii);
    EXPECT_EQ(result->at(0).source, "rr.xml");
    EXPECT_TRUE(result->at(0).parents.empty());
    EXPECT_EQ(result->at(1).definition_id, "CHILD");
    EXPECT_EQ(result->at(1).internal_id_address, 0x1A0U);
    EXPECT_EQ(result->at(1).internal_id, "CHILD-ID");
    EXPECT_EQ(result->at(1).ecu_id, "ECU-1");
    EXPECT_EQ(result->at(1).parents, std::vector<std::string>{"BASE"});
}

TEST(RomRaiderParserTest, ParsesChildWithoutResolvingItsBase)
{
    const auto xml = bytes(R"xml(
      <roms><rom base="BASE"><romid><xmlid>CHILD</xmlid>
      <internalidaddress>100</internalidaddress>
      <internalidstring>ABCD</internalidstring><ecuid>ECU-A</ecuid>
      <make>Subaru</make><market>USDM</market><model>Legacy</model>
      <submodel>GT</submodel><transmission>6MT</transmission><year>2008</year>
      <flashmethod>subaru_denso_can</flashmethod><memmodel>SH7058</memmodel>
      <checksummodule>subarudbw</checksummodule><filesize>1048576</filesize>
      <notes>Golden fixture</notes></romid>
      <table id="fuel-primary" name="Fuel" address="200" type="3D"
             category="Fuel" subcategory="Primary" description="Main fuel map"
             level="2" userlevel="3" sizex="4" sizey="2"
             swapxy="true" flipx="false" flipy="true"
             storagetype="uint16" endian="big" minvalue="0" maxvalue="100">
        <scaling name="fuel-scale" units="%" expression="x*0.5" to_byte="x*2"
                 format="0.0" fineincrement="0.5" coarseincrement="1"/>
        <table type="X Axis" name="Engine Speed" address="300"
               elements="4" storagetype="uint16" endian="big">
          <scaling name="rpm-scale" units="rpm" expression="x" to_byte="x"
                   format="0"/>
        </table>
        <table type="Y Axis" name="Load" storageaddress="400"
               elements="2" storagetype="uint8" endian="little">
          <scaling name="load-scale" units="g/rev" expression="x/10"
                   to_byte="x*10" format="0.0"/>
        </table>
      </table></rom></roms>)xml");

    auto result = parse_romraider_definition(xml, "rr.xml", "CHILD");

    ASSERT_TRUE(result);
    EXPECT_EQ(result->format, DefinitionFormat::RomRaider);
    EXPECT_EQ(result->source, "rr.xml");
    EXPECT_EQ(result->parents, std::vector<std::string>{"BASE"});
    EXPECT_EQ(result->identity.xml_id, "CHILD");
    EXPECT_EQ(result->identity.internal_id_address, 0x100U);
    EXPECT_EQ(result->identity.internal_id, "ABCD");
    EXPECT_EQ(result->identity.ecu_id, "ECU-A");
    EXPECT_EQ(result->metadata.make, "Subaru");
    EXPECT_EQ(result->metadata.market, "USDM");
    EXPECT_EQ(result->metadata.model, "Legacy");
    EXPECT_EQ(result->metadata.submodel, "GT");
    EXPECT_EQ(result->metadata.transmission, "6MT");
    EXPECT_EQ(result->metadata.year, "2008");
    EXPECT_EQ(result->metadata.flash_method, "subaru_denso_can");
    EXPECT_EQ(result->metadata.memory_model, "SH7058");
    EXPECT_EQ(result->metadata.checksum_module, "subarudbw");
    EXPECT_EQ(result->metadata.file_size, "1048576");
    EXPECT_EQ(result->metadata.notes, "Golden fixture");

    ASSERT_EQ(result->maps.size(), 1U);
    const auto& map = result->maps.front();
    EXPECT_EQ(map.id, "fuel-primary");
    EXPECT_EQ(map.name, "Fuel");
    EXPECT_EQ(map.address, 0x200U);
    EXPECT_EQ(map.type, "3D");
    EXPECT_EQ(map.category, "Fuel");
    EXPECT_EQ(map.subcategory, "Primary");
    EXPECT_EQ(map.description, "Main fuel map");
    EXPECT_EQ(map.level, "2");
    EXPECT_EQ(map.user_level, "3");
    EXPECT_EQ(map.x_size, 4U);
    EXPECT_EQ(map.y_size, 2U);
    EXPECT_TRUE(map.swap_xy);
    EXPECT_FALSE(map.flip_x);
    EXPECT_TRUE(map.flip_y);
    EXPECT_EQ(map.storage_type, "uint16");
    EXPECT_EQ(map.endian, "big");
    EXPECT_EQ(map.scaling_name, "fuel-scale");

    EXPECT_EQ(map.x_axis.type, "X Axis");
    EXPECT_EQ(map.x_axis.name, "Engine Speed");
    EXPECT_EQ(map.x_axis.address, 0x300U);
    EXPECT_EQ(map.x_axis.size, 4U);
    EXPECT_EQ(map.x_axis.units, "rpm");
    EXPECT_EQ(map.x_axis.from_byte, "x");
    EXPECT_EQ(map.x_axis.to_byte, "x");
    EXPECT_EQ(map.x_axis.scaling_name, "rpm-scale");
    EXPECT_EQ(map.y_axis.type, "Y Axis");
    EXPECT_EQ(map.y_axis.name, "Load");
    EXPECT_EQ(map.y_axis.address, 0x400U);
    EXPECT_EQ(map.y_axis.size, 2U);
    EXPECT_EQ(map.y_axis.units, "g/rev");
    EXPECT_EQ(map.y_axis.from_byte, "x/10");
    EXPECT_EQ(map.y_axis.to_byte, "x*10");
    EXPECT_EQ(map.y_axis.scaling_name, "load-scale");

    ASSERT_EQ(result->scalings.size(), 3U);
    const auto& scaling = result->scalings.front();
    EXPECT_EQ(scaling.name, "fuel-scale");
    EXPECT_EQ(scaling.units, "%");
    EXPECT_EQ(scaling.from_byte, "x*0.5");
    EXPECT_EQ(scaling.to_byte, "x*2");
    EXPECT_EQ(scaling.format, "0.0");
    EXPECT_EQ(scaling.minimum, "0");
    EXPECT_EQ(scaling.maximum, "100");
    EXPECT_EQ(scaling.fine_increment, "0.5");
    EXPECT_EQ(scaling.coarse_increment, "1");
    EXPECT_EQ(scaling.storage_type, "uint16");
    EXPECT_EQ(scaling.endian, "big");
}

TEST(RomRaiderParserTest, ConvertsSwitchStatesToSelectableScaling)
{
    const auto xml = bytes(R"xml(
      <roms><rom><romid><xmlid>SWITCHES</xmlid></romid>
      <table name="Feature Switch" storageaddress="2A" type="Switch">
        <state name="off" data="00"/><state name="on" data="01"/>
      </table></rom></roms>)xml");

    auto result = parse_romraider_definition(xml, "switches.xml", "SWITCHES");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().type, "Selectable");
    EXPECT_EQ(result->maps.front().storage_type, "bloblist");
    ASSERT_EQ(result->scalings.size(), 1U);
    EXPECT_EQ(result->maps.front().scaling_name, "Feature Switch");
    EXPECT_EQ(result->scalings.front().name, "Feature Switch");
    EXPECT_EQ(result->scalings.front().storage_type, "bloblist");
    EXPECT_EQ(result->scalings.front().selections,
              (std::vector<std::pair<std::string, std::string>>{
                  {"disabled", "00"},
                  {"enabled", "01"},
              }));
}

TEST(RomRaiderParserTest, NormalizesLegacyTwoDimensionalYAxisDimensions)
{
    const auto xml = bytes(R"xml(
      <roms><rom><romid><xmlid>CURVE</xmlid></romid>
      <table name="Curve" type="2D" sizey="4">
        <table type="Y Axis" name="Curve Points" storageaddress="80"/>
      </table></rom></roms>)xml");

    auto result = parse_romraider_definition(xml, "curve.xml", "CURVE");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().x_axis.type, "Y Axis");
    EXPECT_EQ(result->maps.front().x_axis.name, "Curve Points");
    EXPECT_EQ(result->maps.front().x_axis.address, 0x80U);
    EXPECT_EQ(result->maps.front().x_axis.size, 4U);
    EXPECT_EQ(result->maps.front().x_size, 4U);
    EXPECT_EQ(result->maps.front().y_size, 1U);
    EXPECT_TRUE(result->maps.front().y_axis.type.empty());
}

TEST(RomRaiderParserTest, NormalizesLegacyStaticYAxisDimensions)
{
    const auto xml = bytes(R"xml(
      <roms><rom><romid><xmlid>STATIC_CURVE</xmlid></romid>
      <table name="Static Curve" type="3D" sizey="5">
        <table type="Static Y Axis" name="Static Points"/>
      </table></rom></roms>)xml");

    auto result = parse_romraider_definition(xml, "static-curve.xml", "STATIC_CURVE");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().x_axis.type, "Static X Axis");
    EXPECT_EQ(result->maps.front().x_axis.name, "Static Points");
    EXPECT_EQ(result->maps.front().x_axis.size, 5U);
    EXPECT_EQ(result->maps.front().x_size, 5U);
    EXPECT_EQ(result->maps.front().y_size, 1U);
    EXPECT_TRUE(result->maps.front().y_axis.type.empty());
}

TEST(RomRaiderParserTest, UsesAddressBeforeStorageAddressAndDefaultsOptionalFields)
{
    const auto xml = bytes(R"xml(
      <roms><rom base=""><romid><xmlid>MINIMAL</xmlid></romid>
      <table name="Minimal Map" address="20" storageaddress="30"/>
      </rom></roms>)xml");

    auto result = parse_romraider_definition(xml, "minimal.xml", "MINIMAL");

    ASSERT_TRUE(result);
    EXPECT_TRUE(result->parents.empty());
    EXPECT_EQ(result->metadata, RomMetadata{});
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().id, "Minimal Map");
    EXPECT_EQ(result->maps.front().address, 0x20U);
    EXPECT_EQ(result->maps.front().x_size, 1U);
    EXPECT_EQ(result->maps.front().y_size, 1U);
    EXPECT_FALSE(result->maps.front().swap_xy);
    EXPECT_FALSE(result->maps.front().flip_x);
    EXPECT_FALSE(result->maps.front().flip_y);
}

TEST(RomRaiderParserTest, MalformedXmlIsInvalidConfigWithSourceAndDocumentContext)
{
    const auto xml = bytes("<roms><rom>");

    auto result = parse_romraider_definition(xml, "broken.xml", "A");

    expect_invalid_with_context(result, "broken.xml", "XML document");
}

TEST(RomRaiderParserTest, MissingXmlIdIsInvalidConfigWithElementContext)
{
    const auto xml = bytes("<roms><rom><romid><ecuid>E</ecuid></romid></rom></roms>");

    auto result = parse_romraider_definition(xml, "missing-id.xml", "A");

    expect_invalid_with_context(result, "missing-id.xml", "<xmlid>");
}

TEST(RomRaiderParserTest, InvalidAddressIsInvalidConfigWithAttributeContext)
{
    const auto xml = bytes(
        "<roms><rom><romid><xmlid>A</xmlid></romid>"
        "<table name=\"Fuel\" storageaddress=\"not-hex\"/></rom></roms>");

    auto result = parse_romraider_definition(xml, "bad-address.xml", "A");

    expect_invalid_with_context(result, "bad-address.xml", "storageaddress");
}

TEST(RomRaiderParserTest, InvalidBooleanIsInvalidConfigWithAttributeContext)
{
    const auto xml = bytes(
        "<roms><rom><romid><xmlid>A</xmlid></romid>"
        "<table name=\"Fuel\" flipx=\"yes\"/></rom></roms>");

    auto result = parse_romraider_definition(xml, "bad-bool.xml", "A");

    expect_invalid_with_context(result, "bad-bool.xml", "flipx");
}

TEST(RomRaiderParserTest, DuplicateMapIdentityIsInvalidConfigWithTableContext)
{
    const auto xml = bytes(
        "<roms><rom><romid><xmlid>A</xmlid></romid>"
        "<table name=\"Fuel\"/><table name=\"Fuel\"/></rom></roms>");

    auto result = parse_romraider_definition(xml, "duplicate.xml", "A");

    expect_invalid_with_context(result, "duplicate.xml", "<table>");
}

TEST(RomRaiderParserTest, WrongRootIsInvalidConfigWithExpectedRootContext)
{
    const auto xml = bytes("<rom><romid><xmlid>A</xmlid></romid></rom>");

    auto result = parse_romraider_definition(xml, "wrong-root.xml", "A");

    expect_invalid_with_context(result, "wrong-root.xml", "<roms>");
}

TEST(RomRaiderParserTest, PreservesRuntimeRowOffsetsLogParametersAndStaticAxisData)
{
    auto result = parse_romraider_definition(bytes(R"xml(
      <roms><rom><romid><xmlid>ROWS</xmlid></romid>
      <table id="fuel" name="Fuel" type="2D" sizex="3" sizey="1"
             startpos="7" interval="2" logparam="P_MAP">
        <table type="Static X Axis" name="Load" elements="3"
               startpos="9" interval="4" logparam="P_LOAD">
          <data>0.5</data><data>1.0</data><data>1.5</data>
        </table>
      </table></rom></roms>)xml"),
                                             "rows.xml",
                                             "ROWS");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    const auto& map = result->maps.front();
    EXPECT_EQ(map.start_position, "7");
    EXPECT_EQ(map.interval, "2");
    EXPECT_EQ(map.log_parameter, "P_MAP");
    EXPECT_TRUE(map.supplied.start_position);
    EXPECT_TRUE(map.supplied.interval);
    EXPECT_TRUE(map.supplied.log_parameter);
    EXPECT_EQ(map.x_axis.start_position, "9");
    EXPECT_EQ(map.x_axis.interval, "4");
    EXPECT_EQ(map.x_axis.log_parameter, "P_LOAD");
    EXPECT_EQ(map.x_axis.static_data, (std::vector<std::string>{"0.5", "1.0", "1.5"}));
    EXPECT_TRUE(map.x_axis.supplied.start_position);
    EXPECT_TRUE(map.x_axis.supplied.interval);
    EXPECT_TRUE(map.x_axis.supplied.log_parameter);
    EXPECT_TRUE(map.x_axis.supplied.static_data);
}

} // namespace
} // namespace fastecu::definition
