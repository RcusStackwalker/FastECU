#include "src/backend/definition/parser_utils.h"

#include <gtest/gtest.h>

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
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
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
    ASSERT_TRUE(file_root);
    EXPECT_EQ(child_text(file_root->child("romid"), "xmlid"), "Caf\xc3\xa9");

    pugi::xml_document form_document;
    const auto form_root = parse_document_root(form_document,
                                               xml_bytes("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"
                                                         "<rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>"),
                                               "form", pugi::encoding_utf8);
    ASSERT_TRUE(form_root);
    EXPECT_EQ(child_text(form_root->child("romid"), "xmlid"), "Caf\xc3\xa9");
}

TEST(ParserUtilsTest, StrictRootSelectionDoesNotAcceptAuthoringContainerPolicy)
{
    pugi::xml_document document;
    const auto root = parse_root(document, xml_bytes("<roms><rom/></roms>"), "wrong-root.xml", "rom");
    ASSERT_FALSE(root);
    EXPECT_NE(root.error().detail.find("root element <rom>: wrong root; found <roms>"), std::string::npos);
}

} // namespace
} // namespace fastecu::definition
