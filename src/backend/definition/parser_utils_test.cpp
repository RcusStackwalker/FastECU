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
    scaling.storage_type = StorageType::kUint8;
    scaling.endian = "little";

    UnresolvedCalibrationMap unset;
    AdoptInlineScaling(scaling, unset);
    EXPECT_EQ(unset.scaling_name, "inline");
    EXPECT_EQ(unset.storage_type, StorageType::kUint8);
    EXPECT_EQ(unset.endian, "little");

    UnresolvedCalibrationMap set;
    set.scaling_name = "reference";
    set.storage_type = StorageType::kUint16;
    set.endian = "big";
    AdoptInlineScaling(scaling, set);
    EXPECT_EQ(set.scaling_name, "inline");
    EXPECT_EQ(set.storage_type, StorageType::kUint16);
    EXPECT_EQ(set.endian, "big");
}

TEST(ParserUtilsTest, MapScalingFallbackNamePrefersReferenceThenIdThenName)
{
    UnresolvedCalibrationMap map;
    map.name = "Fuel";
    EXPECT_EQ(MapScalingFallbackName(map), "Fuel");
    map.id = "fuel-primary";
    EXPECT_EQ(MapScalingFallbackName(map), "fuel-primary");
    map.scaling_name = "fuel-scale";
    EXPECT_EQ(MapScalingFallbackName(map), "fuel-scale");
}

std::span<const std::uint8_t> XmlBytes(std::string_view xml)
{
    return {reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()};
}

TEST(ParserUtilsTest, DocumentLoadingRetainsParseErrorSourceContext)
{
    pugi::xml_document document;
    const auto result = ParseDocumentRoot(document, XmlBytes("<rom><romid>"), "broken.xml", pugi::encoding_auto);
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
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
    const auto file_root = ParseDocumentRoot(file_document, bytes, "encoded.xml", pugi::encoding_auto);
    ASSERT_THAT(file_root, fastecu::testing::IsOk());
    EXPECT_EQ(ChildText(file_root->child("romid"), "xmlid"), "Caf\xc3\xa9");

    pugi::xml_document form_document;
    const auto form_root = ParseDocumentRoot(form_document,
                                             XmlBytes("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"
                                                      "<rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>"),
                                             "form", pugi::encoding_utf8);
    ASSERT_THAT(form_root, fastecu::testing::IsOk());
    EXPECT_EQ(ChildText(form_root->child("romid"), "xmlid"), "Caf\xc3\xa9");
}

TEST(ParserUtilsTest, StrictRootSelectionDoesNotAcceptAuthoringContainerPolicy)
{
    pugi::xml_document document;
    const auto root = ParseRoot(document, XmlBytes("<roms><rom/></roms>"), "wrong-root.xml", "rom");
    ASSERT_THAT(root, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_NE(root.error().detail.find("root element <rom>: wrong root; found <roms>"), std::string::npos);
}

TEST(ParserUtilsTest, RomHeaderOwnsNormalizedIdentityAndBorrowsTheRomElement)
{
    pugi::xml_document document;
    ASSERT_TRUE(
        document.load_string("<rom><romid><xmlid> ID </xmlid><internalidstring> INTERNAL </internalidstring>"
                             "<ecuid> ECU </ecuid><internalidaddress> 0x20 </internalidaddress></romid></rom>"));
    const auto header = ParseRomHeader(document.document_element(), "identity.xml");
    ASSERT_THAT(header, fastecu::testing::IsOk());
    EXPECT_EQ(header->rom, document.document_element());
    EXPECT_EQ(header->identity, (RomIdentity{.xml_id = "ID",
                                             .internal_id = "INTERNAL",
                                             .ecu_id = "ECU",
                                             .internal_id_address = memory::DefinitionAddress{0x20}}));
}

TEST(ParserUtilsTest, RomHeaderKeepsMissingAddressOptional)
{
    pugi::xml_document document;
    ASSERT_TRUE(document.load_string("<rom><romid><xmlid>ID</xmlid></romid></rom>"));
    const auto header = ParseRomHeader(document.document_element(), "identity.xml");
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
        const auto header = ParseRomHeader(document.document_element(), "identity.xml");
        ASSERT_THAT(header, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        EXPECT_NE(header.error().detail.find("source 'identity.xml'"), std::string::npos);
        EXPECT_NE(header.error().detail.find(context), std::string::npos);
    }
}

TEST(ParserUtilsTest, RomHeaderRejectsInvalidAndOverflowingAddressesWithDefinitionContext)
{
    for (const std::string address : {"bad-address", "10000000000000000"})
    {
        SCOPED_TRACE(address);
        pugi::xml_document document;
        const std::string xml =
            "<rom><romid><xmlid>ID</xmlid><internalidaddress>" + address + "</internalidaddress></romid></rom>";
        ASSERT_TRUE(document.load_string(xml.c_str()));
        const auto header = ParseRomHeader(document.document_element(), "identity.xml");
        ASSERT_THAT(header, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
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
    EXPECT_EQ(ParseMetadata(document.document_element().child("romid")), (RomMetadata{.make = "make",
                                                                                      .market = "market",
                                                                                      .model = "model",
                                                                                      .submodel = "submodel",
                                                                                      .transmission = "transmission",
                                                                                      .year = "year",
                                                                                      .flash_method = "flash",
                                                                                      .memory_model = "memory",
                                                                                      .checksum_module = "checksum",
                                                                                      .file_size = "1024",
                                                                                      .notes = " metadata notes "}));
}

} // namespace

TEST(ParserUtilsTest, HeaderReadsAllDirectTextAndNormalizesUnicodePadding)
{
    pugi::xml_document document;
    ASSERT_TRUE(document.load_string("<rom><romid><xmlid>\xc2\xa0"
                                     "CAL<!-- split -->123<![CDATA[X]]>\xe3\x80\x80</xmlid>"
                                     "<internalidstring> ID </internalidstring><ecuid> ECU </ecuid>"
                                     "<internalidaddress>\xc2\xa0+0X10\xe3\x80\x80</internalidaddress>"
                                     "<model> Mitsubishi Colt </model><notes> \nnotes \n </notes></romid></rom>",
                                     pugi::parse_default | pugi::parse_comments));
    const auto header = ParseRomHeader(document.document_element(), "header.xml");
    ASSERT_THAT(header, fastecu::testing::IsOk());
    EXPECT_EQ(header->identity.xml_id, "CAL123X");
    EXPECT_EQ(header->identity.internal_id_address, memory::DefinitionAddress{16});
    const auto metadata = ParseMetadata(header->rom.child("romid"));
    EXPECT_EQ(metadata.model, "Mitsubishi Colt");
    EXPECT_EQ(metadata.notes, " \nnotes \n ");
}

TEST(ParserUtilsTest, HeaderTreatsBlankAddressAsAbsent)
{
    pugi::xml_document document;
    ASSERT_TRUE(document.load_string("<rom><romid><xmlid>ID</xmlid>"
                                     "<internalidaddress>\xc2\xa0 </internalidaddress></romid></rom>"));
    const auto header = ParseRomHeader(document.document_element(), "header.xml");
    ASSERT_THAT(header, fastecu::testing::IsOk());
    EXPECT_EQ(header->identity.internal_id_address, std::nullopt);
}

TEST(ParserUtilsTest, HeaderRejectsNestedScalarContentWithSourceContext)
{
    for (const std::string field : {"xmlid", "internalidstring", "model", "notes"})
    {
        pugi::xml_document document;
        const std::string xml = "<rom><romid><" + field + ">A<b>B</b></" + field + ">" +
                                (field == "xmlid" ? "" : "<xmlid>ID</xmlid>") + "</romid></rom>";
        ASSERT_TRUE(document.load_string(xml.c_str()));
        const auto header = ParseRomHeader(document.document_element(), "nested.xml");
        ASSERT_THAT(header, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        EXPECT_THAT(header.error().detail, ::testing::HasSubstr("nested.xml"));
        EXPECT_THAT(header.error().detail, ::testing::HasSubstr(field));
    }
}

TEST(ParserUtilsTest, DocumentLoadingRejectsMultipleRootsForStrictLoadersToo)
{
    pugi::xml_document document;
    EXPECT_THAT(ParseDocumentRoot(document, XmlBytes("<rom/><rom/>"), "roots.xml", pugi::encoding_auto),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

} // namespace fastecu::definition
