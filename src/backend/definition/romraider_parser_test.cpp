#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/definition/romraider_parser.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

std::vector<std::uint8_t> Bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

void ExpectInvalidWithContext(const Result<UnresolvedDefinition>& result, std::string_view source_context,
                              std::string_view xml_context)
{
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr(source_context));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr(xml_context));
}

TEST(RomRaiderParserTest, IndexesMultipleDefinitionsAndRecordsParentReferences)
{
    const auto xml = Bytes(R"xml(
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

    auto result = ParseRomraiderIndex(xml, "rr.xml");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(result->at(0).definition_id, "BASE");
    EXPECT_EQ(result->at(0).internal_id, "BASE-ID");
    EXPECT_EQ(result->at(0).internal_id_encoding, IdEncoding::kAsciiOrHex);
    EXPECT_EQ(result->at(0).source, "rr.xml");
    EXPECT_TRUE(result->at(0).parents.empty());
    EXPECT_EQ(result->at(1).definition_id, "CHILD");
    EXPECT_EQ(result->at(1).internal_id_address, memory::DefinitionAddress{0x1A0});
    EXPECT_EQ(result->at(1).internal_id, "CHILD-ID");
    EXPECT_EQ(result->at(1).ecu_id, "ECU-1");
    EXPECT_EQ(result->at(1).parents, std::vector<std::string>{"BASE"});
}

TEST(RomRaiderParserTest, ParsesChildWithoutResolvingItsBase)
{
    const auto xml = Bytes(R"xml(
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
             storagetype="uint16" endian="big" minvalue="0" maxvalue="100"
             startpos="12" interval="3" logparam="fuel">
        <scaling name="fuel-scale" units="%" expression="x*0.5" to_byte="x*2"
                 format="0.0" fineincrement="0.5" coarseincrement="1"/>
        <table type="X Axis" name="Engine Speed" address="300"
               elements="4" storagetype="uint16" endian="big"
               startpos="21" interval="5" logparam="rpm">
          <scaling name="rpm-scale" units="rpm" expression="x" to_byte="x"
                   format="0"/>
        </table>
        <table type="Y Axis" name="Load" storageaddress="400"
               elements="2" storagetype="uint8" endian="little">
          <scaling name="load-scale" units="g/rev" expression="x/10"
                   to_byte="x*10" format="0.0"/>
        </table>
      </table></rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "rr.xml", "CHILD");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->format, DefinitionFormat::kRomRaider);
    EXPECT_EQ(result->source, "rr.xml");
    EXPECT_EQ(result->parents, std::vector<std::string>{"BASE"});
    EXPECT_EQ(result->identity.xml_id, "CHILD");
    EXPECT_EQ(result->identity.internal_id_address, memory::DefinitionAddress{0x100});
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
    EXPECT_EQ(map.address, memory::DefinitionAddress{0x200});
    EXPECT_EQ(map.type, "3D");
    EXPECT_EQ(map.category, "Fuel");
    EXPECT_EQ(map.subcategory, "Primary");
    EXPECT_EQ(map.description, "Main fuel map");
    EXPECT_EQ(map.level, "2");
    EXPECT_EQ(map.user_level, "3");
    EXPECT_EQ(map.x_size, 4U);
    EXPECT_EQ(map.y_size, 2U);
    EXPECT_EQ(map.swap_xy, true);
    EXPECT_EQ(map.flip_x, false);
    EXPECT_EQ(map.flip_y, true);
    EXPECT_EQ(map.storage_type, StorageType::kUint16);
    EXPECT_EQ(map.endian, "big");
    EXPECT_EQ(map.scaling_name, "fuel-scale");
    EXPECT_EQ(map.start_position, 0x12U);
    EXPECT_EQ(map.interval, 0x3U);
    EXPECT_EQ(map.log_parameter, "fuel");

    EXPECT_EQ(map.x_axis.type, "X Axis");
    EXPECT_EQ(map.x_axis.name, "Engine Speed");
    EXPECT_EQ(map.x_axis.address, memory::DefinitionAddress{0x300});
    EXPECT_EQ(map.x_axis.size, 4U);
    EXPECT_EQ(map.x_axis.units, "rpm");
    EXPECT_EQ(map.x_axis.from_byte, "x");
    EXPECT_EQ(map.x_axis.to_byte, "x");
    EXPECT_EQ(map.x_axis.scaling_name, "rpm-scale");
    EXPECT_EQ(map.x_axis.start_position, 0x21U);
    EXPECT_EQ(map.x_axis.interval, 0x5U);
    EXPECT_EQ(map.x_axis.log_parameter, "rpm");
    EXPECT_EQ(map.y_axis.type, "Y Axis");
    EXPECT_EQ(map.y_axis.name, "Load");
    EXPECT_EQ(map.y_axis.address, memory::DefinitionAddress{0x400});
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
    EXPECT_EQ(scaling.storage_type, StorageType::kUint16);
    EXPECT_EQ(scaling.endian, "big");
}

TEST(RomRaiderParserTest, ConvertsSwitchStatesToSelectableScaling)
{
    const auto xml = Bytes(R"xml(
      <roms><rom><romid><xmlid>SWITCHES</xmlid></romid>
      <table name="Feature Switch" storageaddress="2A" type="Switch">
        <state name="off" data="00"/><state name="on" data="01"/>
      </table></rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "switches.xml", "SWITCHES");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().type, "Selectable");
    EXPECT_EQ(result->maps.front().storage_type, StorageType::kBloblist);
    ASSERT_EQ(result->scalings.size(), 1U);
    EXPECT_EQ(result->maps.front().scaling_name, "Feature Switch");
    EXPECT_EQ(result->scalings.front().name, "Feature Switch");
    EXPECT_EQ(result->scalings.front().storage_type, StorageType::kBloblist);
    EXPECT_EQ(result->scalings.front().selections, (std::vector<std::pair<std::string, std::string>>{
                                                       {"disabled", "00"},
                                                       {"enabled", "01"},
                                                   }));
}

TEST(RomRaiderParserTest, SwitchWithInlineScalingAppendsTableStatesAfterScalingData)
{
    const auto xml = Bytes(R"xml(
      <roms><rom><romid><xmlid>SWITCHES</xmlid></romid>
      <table name="Feature Switch" storageaddress="2A" type="Switch">
        <scaling name="switch-scale" storagetype="uint8"><data name="auto" value="02"/></scaling>
        <state name="off" data="00"/><state name="on" data="01"/>
      </table></rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "switches.xml", "SWITCHES");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().type, "Selectable");
    EXPECT_EQ(result->maps.front().storage_type, StorageType::kBloblist);
    EXPECT_EQ(result->maps.front().scaling_name, "switch-scale");
    ASSERT_EQ(result->scalings.size(), 1U);
    EXPECT_EQ(result->scalings.front().storage_type, StorageType::kBloblist);
    EXPECT_EQ(result->scalings.front().selections, (std::vector<std::pair<std::string, std::string>>{
                                                       {"auto", "02"},
                                                       {"disabled", "00"},
                                                       {"enabled", "01"},
                                                   }));
}

TEST(RomRaiderParserTest, AxisScalingFallsBackToTheAxisTableAttributes)
{
    const auto xml = Bytes(R"xml(
      <roms><rom><romid><xmlid>FALLBACK</xmlid></romid>
      <table name="Fuel" type="2D" sizey="2">
        <table type="Y Axis" name="Load" elements="2" storagetype="uint8" endian="little"
               minvalue="1" maxvalue="9">
          <scaling units="g/rev" expression="x"/>
        </table>
      </table></rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "fallback.xml", "FALLBACK");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->scalings.size(), 1U);
    const auto& scaling = result->scalings.front();
    EXPECT_EQ(scaling.name, "Load");
    EXPECT_EQ(scaling.storage_type, StorageType::kUint8);
    EXPECT_EQ(scaling.endian, "little");
    EXPECT_EQ(scaling.minimum, "1");
    EXPECT_EQ(scaling.maximum, "9");
}

TEST(RomRaiderParserTest, AxisWithoutANameIsInvalidConfigNamingTheAxisType)
{
    auto result = ParseRomraiderDefinition(Bytes(R"xml(
      <roms><rom><romid><xmlid>A</xmlid></romid><table name="Fuel" type="3D">
      <table type="X Axis"/></table></rom></roms>)xml"),
                                           "bad-axis.xml", "A");
    ExpectInvalidWithContext(result, "bad-axis.xml", "type 'X Axis'");
}

TEST(RomRaiderParserTest, NormalizesLegacyTwoDimensionalYAxisDimensions)
{
    const auto xml = Bytes(R"xml(
      <roms><rom><romid><xmlid>CURVE</xmlid></romid>
      <table name="Curve" type="2D" sizey="4">
        <table type="Y Axis" name="Curve Points" storageaddress="80"/>
      </table></rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "curve.xml", "CURVE");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().x_axis.type, "Y Axis");
    EXPECT_EQ(result->maps.front().x_axis.name, "Curve Points");
    EXPECT_EQ(result->maps.front().x_axis.address, memory::DefinitionAddress{0x80});
    EXPECT_EQ(result->maps.front().x_axis.size, 4U);
    EXPECT_EQ(result->maps.front().x_size, 4U);
    EXPECT_EQ(result->maps.front().y_size, 1U);
    EXPECT_TRUE(result->maps.front().y_axis.type.empty());
}

TEST(RomRaiderParserTest, NormalizesLegacyStaticYAxisDimensions)
{
    const auto xml = Bytes(R"xml(
      <roms><rom><romid><xmlid>STATIC_CURVE</xmlid></romid>
      <table name="Static Curve" type="3D" sizey="5">
        <table type="Static Y Axis" name="Static Points"/>
      </table></rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "static-curve.xml", "STATIC_CURVE");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().x_axis.type, "Static X Axis");
    EXPECT_EQ(result->maps.front().x_axis.name, "Static Points");
    EXPECT_EQ(result->maps.front().x_axis.size, 5U);
    EXPECT_EQ(result->maps.front().x_size, 5U);
    EXPECT_EQ(result->maps.front().y_size, 1U);
    EXPECT_TRUE(result->maps.front().y_axis.type.empty());
}

TEST(RomRaiderParserTest, UsesAddressBeforeStorageAddressAndPreservesAbsentOptionalFields)
{
    const auto xml = Bytes(R"xml(
      <roms><rom base=""><romid><xmlid>MINIMAL</xmlid></romid>
      <table name="Minimal Map" address="20" storageaddress="30"/>
      </rom></roms>)xml");

    auto result = ParseRomraiderDefinition(xml, "minimal.xml", "MINIMAL");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(result->parents.empty());
    EXPECT_EQ(result->metadata, RomMetadata{});
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_FALSE(result->maps.front().id);
    EXPECT_EQ(result->maps.front().address, memory::DefinitionAddress{0x20});
    EXPECT_FALSE(result->maps.front().x_size);
    EXPECT_FALSE(result->maps.front().y_size);
    EXPECT_FALSE(result->maps.front().swap_xy);
    EXPECT_FALSE(result->maps.front().flip_x);
    EXPECT_FALSE(result->maps.front().flip_y);
}

TEST(RomRaiderParserTest, MalformedXmlIsInvalidConfigWithSourceAndDocumentContext)
{
    const auto xml = Bytes("<roms><rom>");

    auto result = ParseRomraiderDefinition(xml, "broken.xml", "A");

    ExpectInvalidWithContext(result, "broken.xml", "XML document");
}

TEST(RomRaiderParserTest, MissingXmlIdIsInvalidConfigWithElementContext)
{
    const auto xml = Bytes("<roms><rom><romid><ecuid>E</ecuid></romid></rom></roms>");

    auto result = ParseRomraiderDefinition(xml, "missing-id.xml", "A");

    ExpectInvalidWithContext(result, "missing-id.xml", "<xmlid>");
}

TEST(RomRaiderParserTest, InvalidAddressIsInvalidConfigWithAttributeContext)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>A</xmlid></romid>"
                           "<table name=\"Fuel\" storageaddress=\"not-hex\"/></rom></roms>");

    auto result = ParseRomraiderDefinition(xml, "bad-address.xml", "A");

    ExpectInvalidWithContext(result, "bad-address.xml", "storageaddress");
}

TEST(RomRaiderParserTest, InvalidStartPositionOrIntervalIsInvalidConfigWithAttributeContext)
{
    for (const auto attribute : {"startpos", "interval"})
    {
        const std::string xml = std::string("<roms><rom><romid><xmlid>A</xmlid></romid>"
                                            "<table name=\"Fuel\" ") +
                                attribute + "=\"not-hex\"/></rom></roms>";

        auto result = ParseRomraiderDefinition(Bytes(xml), "bad-hex-dimension.xml", "A");

        ExpectInvalidWithContext(result, "bad-hex-dimension.xml", attribute);
    }
}

TEST(RomRaiderParserTest, UnrecognizedStorageTypeIsInvalidConfigWithAttributeContext)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>A</xmlid></romid>"
                           "<table name=\"Fuel\" storagetype=\"nibble\"/></rom></roms>");

    auto result = ParseRomraiderDefinition(xml, "bad-storagetype.xml", "A");

    ExpectInvalidWithContext(result, "bad-storagetype.xml", "storagetype");
}

TEST(RomRaiderParserTest, InvalidBooleanIsInvalidConfigWithAttributeContext)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>A</xmlid></romid>"
                           "<table name=\"Fuel\" flipx=\"yes\"/></rom></roms>");

    auto result = ParseRomraiderDefinition(xml, "bad-bool.xml", "A");

    ExpectInvalidWithContext(result, "bad-bool.xml", "flipx");
}

TEST(RomRaiderParserTest, DuplicateMapIdentityIsInvalidConfigWithTableContext)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>A</xmlid></romid>"
                           "<table name=\"Fuel\"/><table name=\"Fuel\"/></rom></roms>");

    auto result = ParseRomraiderDefinition(xml, "duplicate.xml", "A");

    ExpectInvalidWithContext(result, "duplicate.xml", "<table>");
}

TEST(RomRaiderParserTest, RejectsSecondAxisTargetingAnOccupiedSemanticSlot)
{
    auto duplicate_x = ParseRomraiderDefinition(Bytes(R"xml(
      <roms><rom><romid><xmlid>X_DUPLICATE</xmlid></romid>
      <table name="Fuel" type="3D" sizex="2" sizey="2">
        <table type="X Axis" name="First X"/>
        <table type="Static X Axis" name="Second X"/>
        <table type="Y Axis" name="Only Y"/>
      </table></rom></roms>)xml"),
                                                "duplicate-x-axis.xml", "X_DUPLICATE");
    ExpectInvalidWithContext(duplicate_x, "duplicate-x-axis.xml", "X axis");

    auto duplicate_y = ParseRomraiderDefinition(Bytes(R"xml(
      <roms><rom><romid><xmlid>Y_DUPLICATE</xmlid></romid>
      <table name="Fuel" type="3D" sizex="2" sizey="2">
        <table type="X Axis" name="Only X"/>
        <table type="Y Axis" name="First Y"/>
        <table type="Y Axis" name="Second Y"/>
      </table></rom></roms>)xml"),
                                                "duplicate-y-axis.xml", "Y_DUPLICATE");
    ExpectInvalidWithContext(duplicate_y, "duplicate-y-axis.xml", "Y axis");
}

TEST(RomRaiderParserTest, WrongRootIsInvalidConfigWithExpectedRootContext)
{
    const auto xml = Bytes("<rom><romid><xmlid>A</xmlid></romid></rom>");

    auto result = ParseRomraiderDefinition(xml, "wrong-root.xml", "A");

    ExpectInvalidWithContext(result, "wrong-root.xml", "<roms>");
}

TEST(RomRaiderParserTest, UnselectedInvalidAddressDoesNotBlockRequestedDefinitionButFailsIndexing)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>OTHER</xmlid>"
                           "<internalidaddress>invalid</internalidaddress></romid></rom>"
                           "<rom base=\"BASE\"><romid><xmlid>SELECTED</xmlid>"
                           "<internalidaddress>20</internalidaddress></romid></rom></roms>");
    const auto definition = ParseRomraiderDefinition(xml, "selection.xml", "SELECTED");
    ASSERT_THAT(definition, fastecu::testing::IsOk());
    EXPECT_EQ(definition->identity.xml_id, "SELECTED");
    EXPECT_EQ(definition->identity.internal_id_address, memory::DefinitionAddress{0x20});
    EXPECT_EQ(definition->parents, (std::vector<std::string>{"BASE"}));

    const auto index = ParseRomraiderIndex(xml, "selection.xml");
    ASSERT_THAT(index, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(index.error().detail, ::testing::HasSubstr("definition 'OTHER'"));
    EXPECT_THAT(index.error().detail, ::testing::HasSubstr("internalidaddress"));
}

TEST(RomRaiderParserTest, DuplicateRequestedIdentityIsReportedBeforeParsingItsAddress)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>SELECTED</xmlid>"
                           "<internalidaddress>invalid</internalidaddress></romid></rom>"
                           "<rom><romid><xmlid>SELECTED</xmlid></romid></rom></roms>");
    const auto definition = ParseRomraiderDefinition(xml, "duplicate-id.xml", "SELECTED");
    ASSERT_THAT(definition, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(definition.error().detail, ::testing::HasSubstr("definition 'SELECTED'"));
    EXPECT_THAT(definition.error().detail, ::testing::HasSubstr("duplicate definition identity"));
}

TEST(RomRaiderParserTest, UnknownIdentityIsReportedWithoutParsingOtherRecordAddresses)
{
    const auto xml = Bytes("<roms><rom><romid><xmlid>OTHER</xmlid>"
                           "<internalidaddress>invalid</internalidaddress></romid></rom></roms>");
    const auto definition = ParseRomraiderDefinition(xml, "unknown-id.xml", "MISSING");
    ASSERT_THAT(definition, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(definition.error().detail, ::testing::HasSubstr("definition 'MISSING'"));
    EXPECT_THAT(definition.error().detail, ::testing::HasSubstr("definition ID not found"));
}

TEST(RomRaiderParserTest, HeaderWhitespacePreservationDoesNotHideTableDescriptionOrStaticData)
{
    const auto xml = Bytes(R"xml(<roms><rom><romid><xmlid>ID</xmlid><notes>  </notes></romid>
      <table name="Fuel" address="100"><description> <!-- split -->description</description>
        <table type="Static X Axis" name="RPM" elements="1"><data> <![CDATA[1000]]></data></table>
      </table>
      <table name="Inline" address="200"><data> <![CDATA[2000]]></data></table>
    </rom></roms>)xml");
    const auto result = ParseRomraiderDefinition(xml, "whitespace.xml", "ID");
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->maps.size(), 2U);
    EXPECT_EQ(result->maps[0].description, "description");
    EXPECT_THAT(result->maps[0].x_axis.static_data, ::testing::Optional(::testing::ElementsAre("1000")));

    EXPECT_EQ(result->metadata.notes, "  ");
}

TEST(RomRaiderParserTest, AnAxisAddressWiderThan32BitsIsAParseError)
{
    const auto xml = Bytes(R"xml(<roms><rom><romid><xmlid>WIDE</xmlid></romid>
      <table name="Fuel" type="2D" storageaddress="10" storagetype="uint8">
        <table type="X Axis" name="RPM" storageaddress="100000000" storagetype="uint8" sizex="2"/>
      </table></rom></roms>)xml");

    const auto parsed = ParseRomraiderDefinition(xml, "wide.xml", "WIDE");

    ASSERT_THAT(parsed, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(parsed.error().detail, ::testing::HasSubstr("does not fit 32 bits"));
}

} // namespace
} // namespace fastecu::definition
