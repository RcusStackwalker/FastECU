#include "src/backend/definition/ecuflash_parser.h"

#include <cstdint>
#include <span>
#include <string>
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

TEST(EcuFlashParserTest, IndexesIdentityAndIncludeWithoutResolvingIt)
{
    const auto xml = bytes(R"xml(
      <rom><romid><xmlid>CHILD</xmlid><internalidaddress>1A0</internalidaddress>
      <internalidstring>CHILD-ID</internalidstring><ecuid>ECU-1</ecuid></romid>
      <include>BASE</include></rom>)xml");

    auto result = parse_ecuflash_index(xml, "ecuflash.xml");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().format, DefinitionFormat::EcuFlash);
    EXPECT_EQ(result->front().definition_id, "CHILD");
    EXPECT_EQ(result->front().internal_id_address, 0x1A0U);
    EXPECT_EQ(result->front().internal_id, "CHILD-ID");
    EXPECT_EQ(result->front().internal_id_encoding, IdEncoding::Ascii);
    EXPECT_EQ(result->front().ecu_id, "ECU-1");
    EXPECT_EQ(result->front().source, "ecuflash.xml");
    EXPECT_EQ(result->front().parents, std::vector<std::string>{"BASE"});
}

TEST(EcuFlashParserTest, ParsesMetadataGlobalScalingsAndNestedAxes)
{
    const auto xml = bytes(R"xml(
      <rom>
        <romid><xmlid>TEST</xmlid><internalidaddress>100</internalidaddress>
          <internalidstring>TEST-ID</internalidstring><ecuid>ECU-A</ecuid>
          <make>Subaru</make><market>USDM</market><model>Legacy</model>
          <submodel>GT</submodel><transmission>6MT</transmission><year>2008</year>
          <flashmethod>denso</flashmethod><memmodel>SH7058</memmodel>
          <checksummodule>subaru</checksummodule><filesize>1048576</filesize>
          <notes>Golden fixture</notes></romid>
        <include>BASE</include>
        <scaling name="fuel-scale" units="%" toexpr="x*0.5" frexpr="x*2"
                 format="%.1f" min="0" max="100" inc="1" storagetype="uint16" endian="big"/>
        <scaling name="mode-scale" storagetype="bloblist"><data name="off" value="00"/>
                 <data name="on" data="01"/></scaling>
        <table id="fuel-primary" name="Fuel" address="200" type="3D"
               category="Fuel" subcategory="Primary" description="Main fuel map"
               level="2" userlevel="3" sizex="4" sizey="2" swapxy="true"
               flipx="false" flipy="true" scaling="fuel-scale" storagetype="uint16" endian="big">
          <table type="X Axis" name="Engine Speed" address="300" elements="4" scaling="rpm-scale">
            <scaling name="rpm-scale" units="rpm" toexpr="x" frexpr="x" format="%f"/>
          </table>
          <table type="Y Axis" name="Load" storageaddress="400" elements="2" scaling="load-scale">
            <scaling name="load-scale" units="g/rev" toexpr="x/10" frexpr="x*10" format="%.2f"/>
          </table>
        </table>
      </rom>)xml");

    auto result = parse_ecuflash_definition(xml, "test.xml");

    ASSERT_TRUE(result);
    EXPECT_EQ(result->format, DefinitionFormat::EcuFlash);
    EXPECT_EQ(result->source, "test.xml");
    EXPECT_EQ(result->parents, std::vector<std::string>{"BASE"});
    EXPECT_EQ(result->identity.xml_id, "TEST");
    EXPECT_EQ(result->identity.internal_id_address, 0x100U);
    EXPECT_EQ(result->identity.internal_id, "TEST-ID");
    EXPECT_EQ(result->identity.ecu_id, "ECU-A");
    EXPECT_EQ(result->metadata.make, "Subaru");
    EXPECT_EQ(result->metadata.market, "USDM");
    EXPECT_EQ(result->metadata.model, "Legacy");
    EXPECT_EQ(result->metadata.submodel, "GT");
    EXPECT_EQ(result->metadata.transmission, "6MT");
    EXPECT_EQ(result->metadata.year, "2008");
    EXPECT_EQ(result->metadata.flash_method, "denso");
    EXPECT_EQ(result->metadata.memory_model, "SH7058");
    EXPECT_EQ(result->metadata.checksum_module, "subaru");
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
    EXPECT_EQ(map.scaling_name, "fuel-scale");
    EXPECT_EQ(map.x_axis.type, "X Axis");
    EXPECT_EQ(map.x_axis.name, "Engine Speed");
    EXPECT_EQ(map.x_axis.address, 0x300U);
    EXPECT_EQ(map.x_axis.size, 4U);
    EXPECT_EQ(map.x_axis.units, "rpm");
    EXPECT_EQ(map.x_axis.scaling_name, "rpm-scale");
    EXPECT_EQ(map.y_axis.type, "Y Axis");
    EXPECT_EQ(map.y_axis.name, "Load");
    EXPECT_EQ(map.y_axis.address, 0x400U);
    EXPECT_EQ(map.y_axis.size, 2U);
    EXPECT_EQ(map.y_axis.units, "g/rev");
    EXPECT_EQ(map.y_axis.scaling_name, "load-scale");

    ASSERT_EQ(result->scalings.size(), 4U);
    const auto& fuel_scale = result->scalings.at(0);
    EXPECT_EQ(fuel_scale.name, "fuel-scale");
    EXPECT_EQ(fuel_scale.from_byte, "x*0.5");
    EXPECT_EQ(fuel_scale.to_byte, "x*2");
    EXPECT_EQ(fuel_scale.format, "0.0");
    EXPECT_EQ(fuel_scale.minimum, "0");
    EXPECT_EQ(fuel_scale.maximum, "100");
    EXPECT_EQ(fuel_scale.coarse_increment, "1");
    EXPECT_EQ(fuel_scale.fine_increment, "0.1");
    EXPECT_EQ(fuel_scale.storage_type, "uint16");
    EXPECT_EQ(fuel_scale.endian, "big");
    EXPECT_EQ(result->scalings.at(1).selections,
              (std::vector<std::pair<std::string, std::string>>{{"disabled", "00"}, {"enabled", "01"}}));
    EXPECT_EQ(result->scalings.at(2).format, "0");
    EXPECT_EQ(result->scalings.at(3).format, "0.00");
}

TEST(EcuFlashParserTest, AddressWinsAndStrictFlagsParse)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>TEST</xmlid></romid>
      <table name="Fuel" address="1000" storageaddress="2000"
             swapxy="true" flipx="false" flipy="true"/></rom>)xml"),
                                            "test.xml");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->maps.at(0).address, 0x1000);
    EXPECT_TRUE(result->maps.at(0).swap_xy);
    EXPECT_FALSE(result->maps.at(0).flip_x);
    EXPECT_TRUE(result->maps.at(0).flip_y);
}

TEST(EcuFlashParserTest, NormalizesTopLevelXAxisMapToTwoDimensional)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>TEST</xmlid></romid>
      <table name="Engine Speed" type="X Axis" elements="4"/></rom>)xml"),
                                            "test.xml");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().type, "2D");
    EXPECT_EQ(result->maps.front().x_size, 4U);
    EXPECT_EQ(result->maps.front().y_size, 1U);
}

TEST(EcuFlashParserTest, NormalizesTopLevelYAxisMapToTwoDimensional)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>TEST</xmlid></romid>
      <table name="Load" type="Y Axis" elements="5"/></rom>)xml"),
                                            "test.xml");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().type, "2D");
    EXPECT_EQ(result->maps.front().x_size, 1U);
    EXPECT_EQ(result->maps.front().y_size, 5U);
}

TEST(EcuFlashParserTest, KeepsInputBytesAndSymbolicScalingReferencesUnchanged)
{
    const auto xml = bytes(R"xml(
      <rom><romid><xmlid>TEST</xmlid></romid>
      <scaling name="shared" format="%.0f"/>
      <table name="Fuel" scaling="shared"/></rom>)xml");
    const auto original = xml;

    auto result = parse_ecuflash_definition(xml, "test.xml");

    ASSERT_TRUE(result);
    EXPECT_EQ(xml, original);
    EXPECT_EQ(result->maps.front().scaling_name, "shared");
    EXPECT_EQ(result->scalings.front().name, "shared");
    EXPECT_EQ(result->scalings.front().format, "0");
}

TEST(EcuFlashParserTest, ConvertsAnyPositivePrintfPrecision)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>TEST</xmlid></romid>
      <scaling name="precise" format="%.1001f"/></rom>)xml"),
                                            "test.xml");

    ASSERT_TRUE(result);
    EXPECT_EQ(result->scalings.front().format, std::string("0.") + std::string(1001, '0'));
}

TEST(EcuFlashParserTest, RejectsMalformedXml)
{
    auto result = parse_ecuflash_definition(bytes("<rom><romid>"), "broken.xml");
    expect_invalid_with_context(result, "broken.xml", "XML document");
}

TEST(EcuFlashParserTest, RejectsMissingIdentity)
{
    auto result = parse_ecuflash_definition(bytes("<rom><romid><ecuid>E</ecuid></romid></rom>"), "missing-id.xml");
    expect_invalid_with_context(result, "missing-id.xml", "<xmlid>");
}

TEST(EcuFlashParserTest, RejectsInvalidAddressAndDimension)
{
    auto address = parse_ecuflash_definition(
        bytes("<rom><romid><xmlid>A</xmlid></romid><table name=\"Fuel\" address=\"not-hex\"/></rom>"),
        "bad-address.xml");
    expect_invalid_with_context(address, "bad-address.xml", "address");

    auto dimension = parse_ecuflash_definition(
        bytes("<rom><romid><xmlid>A</xmlid></romid><table name=\"Fuel\" sizex=\"0\"/></rom>"),
        "bad-dimension.xml");
    expect_invalid_with_context(dimension, "bad-dimension.xml", "sizex");
}

TEST(EcuFlashParserTest, RejectsInvalidStrictFlags)
{
    for (const auto attribute : {"swapxy", "flipx", "flipy"})
    {
        const std::string xml = std::string("<rom><romid><xmlid>A</xmlid></romid><table name=\"Fuel\" ") +
                                attribute + "=\"yes\"/></rom>";
        auto result = parse_ecuflash_definition(bytes(xml), "bad-bool.xml");
        expect_invalid_with_context(result, "bad-bool.xml", attribute);
    }
}

TEST(EcuFlashParserTest, RejectsConflictingDuplicateGlobalScaling)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>A</xmlid></romid>
      <scaling name="shared" toexpr="x"/><scaling name="shared" toexpr="x+1"/>
      </rom>)xml"),
                                            "duplicate.xml");
    expect_invalid_with_context(result, "duplicate.xml", "<scaling>");
}

TEST(EcuFlashParserTest, RejectsStructurallyIncompleteAxis)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>A</xmlid></romid><table name="Fuel" type="3D">
      <table type="X Axis"/></table></rom>)xml"),
                                            "bad-axis.xml");
    expect_invalid_with_context(result, "bad-axis.xml", "X Axis");
}

TEST(EcuFlashParserTest, PreservesRuntimeRowOffsetsAndStaticAxisData)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>ROWS</xmlid></romid>
      <table id="fuel" name="Fuel" type="2D" sizex="3" sizey="1"
             startpos="7" interval="2">
        <table type="Static X Axis" name="Load" elements="3"
               startpos="9" interval="4">
          <data>0.5</data><data>1.0</data><data>1.5</data>
        </table>
      </table></rom>)xml"),
                                            "rows.xml");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    const auto& map = result->maps.front();
    EXPECT_EQ(map.start_position, "7");
    EXPECT_EQ(map.interval, "2");
    EXPECT_TRUE(map.supplied.start_position);
    EXPECT_TRUE(map.supplied.interval);
    EXPECT_EQ(map.x_axis.start_position, "9");
    EXPECT_EQ(map.x_axis.interval, "4");
    EXPECT_EQ(map.x_axis.static_data, (std::vector<std::string>{"0.5", "1.0", "1.5"}));
    EXPECT_TRUE(map.x_axis.supplied.start_position);
    EXPECT_TRUE(map.x_axis.supplied.interval);
    EXPECT_TRUE(map.x_axis.supplied.static_data);
}

TEST(EcuFlashParserTest, ConvertsDirectMapDataToLegacyStaticXAxis)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>STATIC</xmlid></romid>
      <table id="load" name="Load Breakpoints" type="2D">
        <data>0.5</data><data>1.0</data>
      </table></rom>)xml"),
                                            "static.xml");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    const auto& map = result->maps.front();
    EXPECT_EQ(map.x_size, 2U);
    EXPECT_EQ(map.y_size, 1U);
    EXPECT_EQ(map.x_axis.type, "Static X Axis");
    EXPECT_TRUE(map.x_axis.name.empty());
    EXPECT_EQ(map.x_axis.size, 2U);
    EXPECT_EQ(map.x_axis.start_position, "1");
    EXPECT_EQ(map.x_axis.interval, "1");
    EXPECT_FALSE(map.x_axis.supplied.start_position);
    EXPECT_FALSE(map.x_axis.supplied.interval);
    EXPECT_EQ(map.x_axis.static_data, (std::vector<std::string>{"0.5", "1.0"}));
}

} // namespace
} // namespace fastecu::definition
