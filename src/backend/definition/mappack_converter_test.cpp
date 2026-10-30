#include "src/backend/definition/mappack_converter.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>
#include <pugixml.hpp>
#include <string>
#include <string_view>

namespace fastecu::definition
{
namespace
{
const std::string kHeader =
    "Name;FolderName;DataOrg;Columns;Rows;Fieldvalues.Name;Fieldvalues.Unit;Fieldvalues.Factor;Precision;"
    "Fieldvalues.StartAddr;AxisX.Name;AxisX.DataOrg;AxisX.Unit;AxisX.Factor;AxisX.Precision;AxisX.DataHeader;AxisX."
    "DataAddr;"
    "AxisY.Name;AxisY.DataOrg;AxisY.Unit;AxisY.Factor;AxisY.Precision;AxisY.DataHeader;AxisY.DataAddr;Comment\n";

std::string Csv(std::string dimensions = "2;3", std::string comment = "\"a;quoted \"\"description\"\"\"")
{
    return kHeader + "Boost;Engine;eHiLo;" + dimensions +
           ";Pressure;bar;0,01;2;$1234;RPM;eHiLoHiLo;rpm;2;0;1;$2000;Load;eByte;%;0,5;1;1;$3000;" + comment;
}

TEST(MapPackConverter, ConvertsTablesScalingAndRomAddresses)
{
    auto result = ConvertMappackCsv(Csv(), "ECU123");
    ASSERT_THAT(result, fastecu::testing::IsOk());
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

TEST(MapPackConverter, ConvertsScalarsAndOneDimensionalMaps)
{
    for (const auto *dimensions : {"1;1", "1;3", "2;1"})
    {
        SCOPED_TRACE(dimensions);
        auto result = ConvertMappackCsv(Csv(dimensions, "last field"), "ECU");
        ASSERT_THAT(result, fastecu::testing::IsOk());
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

TEST(MapPackConverter, AcceptsCrLfTrailingDelimiterAndMultilineQuotedFields)
{
    auto input = Csv("2;3", "\"first\nsecond\";") + "\r\n";
    input.replace(input.find('\n'), 1, "\r\n");
    auto result = ConvertMappackCsv(input, "ECU");
    ASSERT_THAT(result, fastecu::testing::IsOk());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    EXPECT_STREQ(doc.child("roms").child("rom").child("table").child("description").text().get(), "first\nsecond");
}

std::string CsvReplacing(std::string_view original, std::string_view replacement)
{
    auto input = Csv();
    input.replace(input.find(original), original.size(), replacement);
    return input;
}

struct InvalidCsvCase
{
    std::string name;
    std::string input;
    std::string row;
    std::string column;
    std::string reason;
};

class InvalidMapPackCsv : public ::testing::TestWithParam<InvalidCsvCase>
{
};

TEST_P(InvalidMapPackCsv, ReportsInvalidConfigWithContext)
{
    const auto& invalid = GetParam();
    EXPECT_THAT(
        ConvertMappackCsv(invalid.input, "ECU"),
        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, ::testing::AllOf(::testing::HasSubstr(invalid.row),
                                                                                ::testing::HasSubstr(invalid.column),
                                                                                ::testing::HasSubstr(invalid.reason))));
}

INSTANTIATE_TEST_SUITE_P(
    Validation, InvalidMapPackCsv,
    ::testing::Values(
        InvalidCsvCase{"EmptyInput", "", "row 1", "column header", "header and map rows"},
        InvalidCsvCase{"MissingRequiredHeader", "Name\nBoost", "row 1", "column FolderName", "missing required header"},
        InvalidCsvCase{"DuplicateHeader", CsvReplacing("FolderName", "Name"), "row 1", "column Name",
                       "duplicate header"},
        InvalidCsvCase{"ShortRecord", kHeader + "short", "row 2", "column record", "field count"},
        InvalidCsvCase{"ExtraNonemptyRecordField", Csv() + ";extra", "row 2", "column record", "field count"},
        InvalidCsvCase{"UnexpectedQuote", Csv("2;3", "unexpected\"quote"), "row 2", "column 25", "unexpected quote"},
        InvalidCsvCase{"TextAfterClosingQuote", Csv("2;3", "\"quoted\"text"), "row 2", "column 25",
                       "after quoted field"},
        InvalidCsvCase{"UnterminatedQuote", Csv("2;3", "\"unterminated"), "row 2", "column 25",
                       "unterminated quoted field"},
        InvalidCsvCase{"ZeroColumns", Csv("0;3"), "row 2", "column Columns", "positive dimension"},
        InvalidCsvCase{"MalformedColumns", Csv("two;3"), "row 2", "column Columns", "positive dimension"},
        InvalidCsvCase{"ZeroRows", Csv("2;0"), "row 2", "column Rows", "positive dimension"},
        InvalidCsvCase{"ZeroFactor", CsvReplacing(";0,01;", ";0;"), "row 2", "column Fieldvalues.Factor",
                       "finite nonzero"},
        InvalidCsvCase{"NanFactor", CsvReplacing(";0,01;", ";nan;"), "row 2", "column Fieldvalues.Factor",
                       "finite nonzero"},
        InvalidCsvCase{"InfiniteFactor", CsvReplacing(";0,01;", ";inf;"), "row 2", "column Fieldvalues.Factor",
                       "finite nonzero"},
        InvalidCsvCase{"MalformedFactor", CsvReplacing(";0,01;", ";oops;"), "row 2", "column Fieldvalues.Factor",
                       "finite nonzero"},
        InvalidCsvCase{"NegativePrecision", CsvReplacing(";0,01;2;", ";0,01;-1;"), "row 2", "column Precision",
                       "between 0 and 100"},
        InvalidCsvCase{"ExcessivePrecision", CsvReplacing(";0,01;2;", ";0,01;101;"), "row 2", "column Precision",
                       "between 0 and 100"},
        InvalidCsvCase{"MalformedAddress", CsvReplacing("$1234", "$oops"), "row 2", "column Fieldvalues.StartAddr",
                       "hexadecimal address"},
        InvalidCsvCase{"AddressOverflow", CsvReplacing("$1234", "0x10000000000000000"), "row 2",
                       "column Fieldvalues.StartAddr", "hexadecimal address"},
        InvalidCsvCase{"MissingXAxisHeader", CsvReplacing("AxisX.Name", "UnusedAxisName"), "row 1", "column AxisX.Name",
                       "missing required axis header"},
        InvalidCsvCase{"NegativeXAxisDataHeader", CsvReplacing(";0;1;$2000", ";0;-1;$2000"), "row 2",
                       "column AxisX.DataHeader", "nonnegative integer"},
        InvalidCsvCase{"MalformedXAxisDataHeader", CsvReplacing(";0;1;$2000", ";0;bad;$2000"), "row 2",
                       "column AxisX.DataHeader", "nonnegative integer"},
        InvalidCsvCase{"InvalidXAxisAddress", CsvReplacing("$2000", "$oops"), "row 2", "column AxisX.DataAddr",
                       "hexadecimal address"},
        InvalidCsvCase{"ZeroXAxisFactor", CsvReplacing(";rpm;2;", ";rpm;0;"), "row 2", "column AxisX.Factor",
                       "finite nonzero"},
        InvalidCsvCase{"ExcessiveXAxisPrecision", CsvReplacing(";rpm;2;0;", ";rpm;2;101;"), "row 2",
                       "column AxisX.Precision", "between 0 and 100"},
        InvalidCsvCase{"InvalidYAxisAddress", CsvReplacing("$3000", "$oops"), "row 2", "column AxisY.DataAddr",
                       "hexadecimal address"},
        InvalidCsvCase{"MalformedYAxisDataHeader", CsvReplacing(";1;1;$3000", ";1;bad;$3000"), "row 2",
                       "column AxisY.DataHeader", "nonnegative integer"}),
    [](const ::testing::TestParamInfo<InvalidCsvCase>& param_info) { return param_info.param.name; });

TEST(MapPackConverter, InvalidThirdRecordReportsItsOwnRow)
{
    const auto input = Csv() + "\n" + CsvReplacing(";0,01;", ";0;").substr(kHeader.size());
    EXPECT_THAT(ConvertMappackCsv(input, "ECU"),
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                            ::testing::AllOf(::testing::HasSubstr("row 3"),
                                                             ::testing::HasSubstr("column Fieldvalues.Factor"),
                                                             ::testing::HasSubstr("finite nonzero"))));
}

TEST(MapPackConverter, AcceptsPrecisionBoundaries)
{
    for (const auto *precision : {"0", "100"})
    {
        SCOPED_TRACE(precision);
        auto result = ConvertMappackCsv(CsvReplacing(";0,01;2;", std::string(";0,01;") + precision + ";"), "ECU");
        ASSERT_THAT(result, fastecu::testing::IsOk());
        pugi::xml_document doc;
        ASSERT_TRUE(doc.load_string(result->c_str()));
        const auto format = doc.child("roms").child("rom").child("table").child("scaling").attribute("format");
        const auto expected = std::string_view(precision) == "0" ? std::string("#") : "#0." + std::string(100, '0');
        EXPECT_STREQ(format.value(), expected.c_str());
    }
}

TEST(MapPackConverter, AcceptsUppercaseHexAddressPrefix)
{
    auto result = ConvertMappackCsv(CsvReplacing("$1234", "0X1234"), "ECU");
    ASSERT_THAT(result, fastecu::testing::IsOk());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    EXPECT_STREQ(doc.child("roms").child("rom").next_sibling("rom").child("table").attribute("storageaddress").value(),
                 "0x1234");
}

TEST(MapPackConverter, AllowsUnusedAxisFieldsToBeEmptyForScalars)
{
    const auto input = kHeader + "Value;Engine;eByte;1;1;Value;unit;1;0;$1234;;;;;;;;;;;;;;;scalar";
    auto result = ConvertMappackCsv(input, "ECU");
    EXPECT_THAT(result, fastecu::testing::IsOk());
}

TEST(MapPackConverter, ConvertsMultipleRowsWithoutSharingDimensions)
{
    const auto input = Csv("2;3", "first") + "\n" + Csv("1;1", "second").substr(kHeader.size());
    auto result = ConvertMappackCsv(input, "ECU");
    ASSERT_THAT(result, fastecu::testing::IsOk());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(result->c_str()));
    auto first = doc.child("roms").child("rom").child("table");
    EXPECT_STREQ(first.attribute("type").value(), "3D");
    auto second = first.next_sibling("table");
    EXPECT_STREQ(second.attribute("type").value(), "2D");
    EXPECT_STREQ(second.child("description").text().get(), "second");
}

TEST(MapPackConverter, PreservesAxisAddressRulesAndLegacyMetadata)
{
    auto input = Csv();
    input.replace(input.find(";1;$2000"), 8, ";0;$2000");
    auto result = ConvertMappackCsv(input, "ECU");
    ASSERT_THAT(result, fastecu::testing::IsOk());
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

    result = ConvertMappackCsv(Csv("1;1"), "ECU");
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(doc.load_string(result->c_str()));
    rom = doc.child("roms").child("rom").next_sibling("rom");
    EXPECT_STREQ(rom.child("table").child("table").attribute("type").value(), "X Axis");
}

TEST(MapPackConverter, RejectsXmlControlCharactersInsteadOfTruncatingValues)
{
    EXPECT_THAT(ConvertMappackCsv(Csv("1;1", std::string("nul\0text", 8)), "ECU"),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ConvertMappackCsv(Csv(), std::string("ECU\0ID", 6)),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ConvertMappackCsv(Csv("1;1", std::string(1, '\x01')), "ECU"),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(MapPackConverter, RejectsBlankEcuId)
{
    const auto is_invalid_ecu_id =
        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, ::testing::AllOf(::testing::HasSubstr("row 1"),
                                                                                ::testing::HasSubstr("column ecu_id"),
                                                                                ::testing::HasSubstr("empty")));
    EXPECT_THAT(ConvertMappackCsv(Csv(), ""), is_invalid_ecu_id);
    EXPECT_THAT(ConvertMappackCsv(Csv(), "\xe3\x80\x80"), is_invalid_ecu_id);
}

} // namespace
} // namespace fastecu::definition
