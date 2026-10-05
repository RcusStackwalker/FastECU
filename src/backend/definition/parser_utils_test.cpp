#include "src/backend/definition/parser_utils.h"

#include <array>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::definition
{
namespace
{

TEST(ParserUtilsTest, AdoptInlineScalingTakesTheNameAndFillsOnlyUnsetStorageAndEndian)
{
    UnresolvedScaling scaling;
    scaling.name = "inline";
    scaling.storage_type = StorageType::Uint8;
    scaling.endian = "little";

    UnresolvedCalibrationMap unset;
    adopt_inline_scaling(scaling, unset);
    EXPECT_EQ(unset.scaling_name, "inline");
    EXPECT_EQ(unset.storage_type, StorageType::Uint8);
    EXPECT_EQ(unset.endian, "little");

    UnresolvedCalibrationMap set;
    set.scaling_name = "reference";
    set.storage_type = StorageType::Uint16;
    set.endian = "big";
    adopt_inline_scaling(scaling, set);
    EXPECT_EQ(set.scaling_name, "inline");
    EXPECT_EQ(set.storage_type, StorageType::Uint16);
    EXPECT_EQ(set.endian, "big");
}

TEST(ParserUtilsTest, MapScalingFallbackNamePrefersReferenceThenIdThenName)
{
    UnresolvedCalibrationMap map;
    map.name = "Fuel";
    EXPECT_EQ(map_scaling_fallback_name(map), "Fuel");
    map.id = "fuel-primary";
    EXPECT_EQ(map_scaling_fallback_name(map), "fuel-primary");
    map.scaling_name = "fuel-scale";
    EXPECT_EQ(map_scaling_fallback_name(map), "fuel-scale");
}

std::span<const std::uint8_t> xml_bytes(std::string_view xml)
{
    return {reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()};
}

TEST(ParserUtilsTest, TextModesPreserveRawValuesWithoutConflatingNestedText)
{
    pugi::xml_document document;
    ASSERT_TRUE(document.load_string("<rom><notes>  first &amp; <b>nested</b><![CDATA[ last ]]></notes></rom>"));
    const auto notes = document.document_element().child("notes");
    EXPECT_EQ(read_element_text(notes, XmlTextMode::FirstText), "  first & ");
    EXPECT_EQ(read_element_text(notes, XmlTextMode::DescendantText), "  first & nested last ");
    EXPECT_EQ(child_text(document.document_element(), "notes"), "first &");
    EXPECT_EQ(read_element_text({}, XmlTextMode::FirstText), "");
    EXPECT_EQ(read_element_text({}, XmlTextMode::DescendantText), "");
}

TEST(ParserUtilsTest, DocumentLoadingRetainsParseErrorSourceContext)
{
    pugi::xml_document document;
    const auto result = parse_document_root(document, xml_bytes("<rom><romid>"), "broken.xml", pugi::encoding_auto);
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_NE(result.error().detail.find("source 'broken.xml': XML document: malformed XML:"), std::string::npos);
}

TEST(ParserUtilsTest, DocumentEncodingDistinguishesFileBytesFromUtf8FormText)
{
    const std::u16string text = u"<rom><romid><xmlid>Caf\u00e9</xmlid></romid></rom>";
    std::vector<std::uint8_t> bytes{0xff, 0xfe};
    for (const char16_t value : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    pugi::xml_document file_document;
    const auto file_root = parse_document_root(file_document, bytes, "encoded.xml", pugi::encoding_auto);
    ASSERT_THAT(file_root, fastecu::testing::IsOk());
    EXPECT_EQ(child_text(file_root->child("romid"), "xmlid"), "Caf\xc3\xa9");

    pugi::xml_document form_document;
    const auto form_root = parse_document_root(form_document,
                                               xml_bytes("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"
                                                         "<rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>"),
                                               "form", pugi::encoding_utf8);
    ASSERT_THAT(form_root, fastecu::testing::IsOk());
    EXPECT_EQ(child_text(form_root->child("romid"), "xmlid"), "Caf\xc3\xa9");
}

TEST(ParserUtilsTest, StrictRootSelectionDoesNotAcceptAuthoringContainerPolicy)
{
    pugi::xml_document document;
    const auto root = parse_root(document, xml_bytes("<roms><rom/></roms>"), "wrong-root.xml", "rom");
    ASSERT_THAT(root, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_NE(root.error().detail.find("root element <rom>: wrong root; found <roms>"), std::string::npos);
}

TEST(ParserUtilsTest, RomHeaderOwnsNormalizedIdentityAndBorrowsTheIdentityElement)
{
    pugi::xml_document document;
    ASSERT_TRUE(
        document.load_string("<rom><romid><xmlid> ID </xmlid><internalidstring> INTERNAL </internalidstring>"
                             "<ecuid> ECU </ecuid><internalidaddress> 0x20 </internalidaddress></romid></rom>"));
    const auto header = parse_rom_header(document.document_element(), "identity.xml");
    ASSERT_THAT(header, fastecu::testing::IsOk());
    EXPECT_EQ(header->rom_id, document.document_element().child("romid"));
    EXPECT_EQ(header->identity,
              (RomIdentity{.xml_id = "ID", .internal_id = "INTERNAL", .ecu_id = "ECU", .internal_id_address = 0x20U}));
}

TEST(ParserUtilsTest, RomHeaderKeepsMissingAddressOptional)
{
    pugi::xml_document document;
    ASSERT_TRUE(document.load_string("<rom><romid><xmlid>ID</xmlid></romid></rom>"));
    const auto header = parse_rom_header(document.document_element(), "identity.xml");
    ASSERT_THAT(header, fastecu::testing::IsOk());
    EXPECT_EQ(header->identity,
              (RomIdentity{.xml_id = "ID", .internal_id = "", .ecu_id = "", .internal_id_address = std::nullopt}));
}

TEST(ParserUtilsTest, RomHeaderRejectsMissingAndDuplicateIdentityElementsWithContext)
{
    for (const auto& [xml, context] : std::to_array<std::pair<std::string_view, std::string_view>>(
             {{"<rom/>", "element <rom> child <romid>: missing required identity element"},
              {"<rom><romid/><romid/></rom>", "duplicate singleton identity element"},
              {"<rom><romid><xmlid>ID</xmlid><xmlid>OTHER</xmlid></romid></rom>",
               "element <romid> child <xmlid>: duplicate"},
              {"<rom><romid><xmlid> </xmlid></romid></rom>",
               "element <romid> child <xmlid>: missing or empty required text"}}))
    {
        SCOPED_TRACE(xml);
        pugi::xml_document document;
        ASSERT_TRUE(document.load_buffer(xml.data(), xml.size()));
        const auto header = parse_rom_header(document.document_element(), "identity.xml");
        ASSERT_THAT(header, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
        EXPECT_NE(header.error().detail.find("source 'identity.xml'"), std::string::npos);
        EXPECT_NE(header.error().detail.find(context), std::string::npos);
    }
}

TEST(ParserUtilsTest, RomHeaderRejectsPresentEmptyInvalidAndOverflowingAddressesWithDefinitionContext)
{
    for (const std::string address : {"", " ", "bad-address", "10000000000000000"})
    {
        SCOPED_TRACE(address);
        pugi::xml_document document;
        const std::string xml =
            "<rom><romid><xmlid>ID</xmlid><internalidaddress>" + address + "</internalidaddress></romid></rom>";
        ASSERT_TRUE(document.load_string(xml.c_str()));
        const auto header = parse_rom_header(document.document_element(), "identity.xml");
        ASSERT_THAT(header, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
        EXPECT_NE(header.error().detail.find(
                      "source 'identity.xml', definition 'ID': element <romid> child <internalidaddress>"),
                  std::string::npos);
        EXPECT_NE(header.error().detail.find("invalid hexadecimal unsigned value"), std::string::npos);
    }
}

TEST(ParserUtilsTest, StrictMetadataNormalizesAllEditableFieldsAndKeepsRomIdExtras)
{
    pugi::xml_document document;
    ASSERT_TRUE(document.load_string(
        "<rom><romid><make> make </make><market> market </market><model> model </model>"
        "<submodel> submodel </submodel><transmission> transmission </transmission><year> year </year>"
        "<flashmethod> flash </flashmethod><memmodel> memory </memmodel><checksummodule> checksum </checksummodule>"
        "<filesize> 1024 </filesize><notes> metadata notes </notes></romid><notes>root notes</notes></rom>"));
    EXPECT_EQ(parse_metadata(document.document_element().child("romid")), (RomMetadata{.make = "make",
                                                                                       .market = "market",
                                                                                       .model = "model",
                                                                                       .submodel = "submodel",
                                                                                       .transmission = "transmission",
                                                                                       .year = "year",
                                                                                       .flash_method = "flash",
                                                                                       .memory_model = "memory",
                                                                                       .checksum_module = "checksum",
                                                                                       .file_size = "1024",
                                                                                       .notes = "metadata notes"}));
}

} // namespace
} // namespace fastecu::definition
