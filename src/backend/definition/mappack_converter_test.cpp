#include "src/backend/definition/mappack_converter.h"

#include <gtest/gtest.h>
#include <pugixml.hpp>
#include <string>

namespace fastecu::definition
{
namespace
{
const std::string kHeader =
    "Name;FolderName;DataOrg;Columns;Rows;Fieldvalues.Name;Fieldvalues.Unit;Fieldvalues.Factor;Precision;"
    "Fieldvalues.StartAddr;AxisX.Name;AxisX.DataOrg;AxisX.Unit;AxisX.Factor;AxisX.Precision;AxisX.DataHeader;AxisX."
    "DataAddr;"
    "AxisY.Name;AxisY.DataOrg;AxisY.Unit;AxisY.Factor;AxisY.Precision;AxisY.DataHeader;AxisY.DataAddr;Comment\n";

std::string csv(std::string dimensions = "2;3", std::string comment = "\"a;quoted \"\"description\"\"\"")
{
    return kHeader + "Boost;Engine;eHiLo;" + dimensions +
           ";Pressure;bar;0,01;2;$1234;RPM;eHiLoHiLo;rpm;2;0;1;$2000;Load;eByte;%;0,5;1;1;$3000;" + comment;
}

TEST(MapPackConverterTest, ConvertsTablesScalingAndRomAddresses)
{
    auto result = convert_mappack_csv(csv(), "ECU123");
    ASSERT_TRUE(result.has_value());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    auto base = doc.child("roms").child("rom");
    auto table = base.child("table");
    EXPECT_STREQ(table.attribute("type").value(), "3D");
    EXPECT_STREQ(table.attribute("storagetype").value(), "uint16");
    EXPECT_STREQ(table.attribute("category").value(), "Engine");
    EXPECT_STREQ(table.child("scaling").attribute("expression").value(), "x*0.01");
    EXPECT_STREQ(table.child("scaling").attribute("to_byte").value(), "x/0.01");
    EXPECT_STREQ(table.child("scaling").attribute("units").value(), "Pressure (bar)");
    EXPECT_STREQ(table.child("scaling").attribute("format").value(), "#0.00");
    EXPECT_STREQ(table.child("description").text().get(), "a;quoted \"description\"");
    auto x = table.child("table");
    EXPECT_STREQ(x.attribute("type").value(), "X Axis");
    EXPECT_STREQ(x.attribute("storagetype").value(), "uint32");
    auto y = x.next_sibling("table");
    EXPECT_STREQ(y.attribute("type").value(), "Y Axis");
    EXPECT_STREQ(y.attribute("storagetype").value(), "uint8");
    EXPECT_STREQ(y.child("scaling").attribute("expression").value(), "x*0.5");
    auto rom = base.next_sibling("rom");
    EXPECT_STREQ(rom.attribute("base").value(), "BASE");
    EXPECT_STREQ(rom.child("romid").child("ecuid").text().get(), "ECU123");
    EXPECT_STREQ(rom.child("table").attribute("storageaddress").value(), "0x1234");
    EXPECT_STREQ(rom.child("table").child("table").attribute("storageaddress").value(), "0x2000");
    EXPECT_STREQ(rom.child("table").child("table").next_sibling("table").attribute("storageaddress").value(), "0x3000");
}

TEST(MapPackConverterTest, ConvertsScalarsAndOneDimensionalMaps)
{
    for (const auto *dimensions : {"1;1", "1;3", "2;1"})
    {
        auto result = convert_mappack_csv(csv(dimensions, "last field"), "ECU");
        ASSERT_TRUE(result.has_value());
        pugi::xml_document doc;
        ASSERT_TRUE(doc.load_string(result->c_str()));
        auto table = doc.child("roms").child("rom").child("table");
        EXPECT_STREQ(table.attribute("type").value(), "2D");
        EXPECT_STREQ(table.child("description").text().get(), "last field");
        if (std::string(dimensions) == "1;1")
        {
            EXPECT_STREQ(table.child("table").attribute("type").value(), "Static Y Axis");
        }
    }
}

TEST(MapPackConverterTest, AcceptsCrLfTrailingDelimiterAndMultilineQuotedFields)
{
    auto input = csv("2;3", "\"first\nsecond\";") + "\r\n";
    input.replace(input.find('\n'), 1, "\r\n");
    auto result = convert_mappack_csv(input, "ECU");
    ASSERT_TRUE(result.has_value());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    EXPECT_STREQ(doc.child("roms").child("rom").child("table").child("description").text().get(), "first\nsecond");
}

TEST(MapPackConverterTest, RejectsMalformedInputWithContext)
{
    for (auto input : {std::string{}, std::string("Name\nBoost"), kHeader + "short", csv("0;3"), csv("two;3"),
                       csv("2;3", "\"unterminated")})
    {
        auto result = convert_mappack_csv(input, "ECU");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
        EXPECT_NE(result.error().detail.find("row"), std::string::npos);
        EXPECT_NE(result.error().detail.find("column"), std::string::npos);
    }
    auto input = csv();
    input.replace(input.find("0,01"), 4, "0");
    EXPECT_FALSE(convert_mappack_csv(input, "ECU").has_value());
    input = csv();
    input.replace(input.find("$1234"), 5, "$oops");
    EXPECT_FALSE(convert_mappack_csv(input, "ECU").has_value());
}
TEST(MapPackConverterTest, AllowsUnusedAxisFieldsToBeEmptyForScalars)
{
    const auto input = kHeader + "Value;Engine;eByte;1;1;Value;unit;1;0;$1234;;;;;;;;;;;;;;;scalar";
    auto result = convert_mappack_csv(input, "ECU");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
}

TEST(MapPackConverterTest, ConvertsMultipleRowsWithoutSharingDimensions)
{
    const auto input = csv("2;3", "first") + "\n" + csv("1;1", "second").substr(kHeader.size());
    auto result = convert_mappack_csv(input, "ECU");
    ASSERT_TRUE(result.has_value());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    auto first = doc.child("roms").child("rom").child("table");
    EXPECT_STREQ(first.attribute("type").value(), "3D");
    auto second = first.next_sibling("table");
    EXPECT_STREQ(second.attribute("type").value(), "2D");
    EXPECT_STREQ(second.child("description").text().get(), "second");
}

TEST(MapPackConverterTest, PreservesAxisAddressRulesAndLegacyMetadata)
{
    auto input = csv();
    input.replace(input.find(";1;$2000"), 8, ";0;$2000");
    auto result = convert_mappack_csv(input, "ECU");
    ASSERT_TRUE(result.has_value());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    auto base = doc.child("roms").child("rom");
    EXPECT_STREQ(base.child("table").child("table").attribute("type").value(), "X Axis");
    auto rom = base.next_sibling("rom");
    EXPECT_STREQ(rom.child("table").child("table").attribute("type").value(), "Y Axis");
    EXPECT_STREQ(rom.child("romid").child("internalidaddress").text().get(), "0x50");
    EXPECT_STREQ(rom.child("romid").child("internalidstring").text().get(), "1037369411P321/C51");
    EXPECT_TRUE(rom.child("romid").child("model"));
    EXPECT_STREQ(rom.child("romid").child("model").text().get(), "");

    result = convert_mappack_csv(csv("1;1"), "ECU");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(doc.load_string(result->c_str()));
    rom = doc.child("roms").child("rom").next_sibling("rom");
    EXPECT_STREQ(rom.child("table").child("table").attribute("type").value(), "X Axis");
}

TEST(MapPackConverterTest, RejectsXmlControlCharactersInsteadOfTruncatingValues)
{
    EXPECT_FALSE(convert_mappack_csv(csv("1;1", std::string("nul\0text", 8)), "ECU").has_value());
    EXPECT_FALSE(convert_mappack_csv(csv(), std::string("ECU\0ID", 6)).has_value());
    EXPECT_FALSE(convert_mappack_csv(csv("1;1", std::string(1, '\x01')), "ECU").has_value());
}

} // namespace
} // namespace fastecu::definition
