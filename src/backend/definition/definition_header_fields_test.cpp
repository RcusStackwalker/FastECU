// Header parsing migrated from src/ui/desktop/definition/definition_header_form.cpp.
#include "src/backend/definition/definition_header_fields.h"

#include <array>
#include <limits>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using fastecu::definition::collect_ecuflash_base_header_fields;
using fastecu::definition::definition_header_input;
using fastecu::definition::DefinitionHeaderFields;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;

TEST(DefinitionHeaderFields, PreservesRequestedOrderAndAbsentFields)
{
    const auto names = std::to_array<std::string>({"xmlid", "model", "internalidaddress", "include", "notes", "xmlid"});
    EXPECT_EQ(
        collect_ecuflash_base_header_fields(
            names, "<rom><romid><xmlid>  BASE  </xmlid><internalidaddress>0x2000</internalidaddress></romid></rom>"),
        (DefinitionHeaderFields{{"xmlid", "  BASE  "},
                                {"model", ""},
                                {"internalidaddress", "0x2000"},
                                {"include", ""},
                                {"notes", ""},
                                {"xmlid", "  BASE  "}}));
}

TEST(DefinitionHeaderFields, ReadsWrappedRomAndConcatenatesNestedText)
{
    const auto names = std::to_array<std::string>({"xmlid", "include", "notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<roms><rom><romid><xmlid>BASE</xmlid></romid><include>OEM_BASE</include>"
                         "<notes>Text &amp; <b>nested</b><![CDATA[ notes]]></notes></rom></roms>"),
              (DefinitionHeaderFields{{"xmlid", "BASE"}, {"include", "OEM_BASE"}, {"notes", "Text & nested notes"}}));
}

TEST(DefinitionHeaderFields, MalformedXmlLeavesEveryRequestedFieldBlank)
{
    const auto names = std::to_array<std::string>({"xmlid", "include", "notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(names, "<rom><romid>"),
              (DefinitionHeaderFields{{"xmlid", ""}, {"include", ""}, {"notes", ""}}));
}

TEST(DefinitionHeaderFields, SelectsFirstDirectRomInRomsContainer)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<roms><!-- comment --><metadata/><rom><romid><xmlid>FIRST</xmlid></romid></rom>"
                         "<rom><romid><xmlid>SECOND</xmlid></romid></rom></roms>"),
              (DefinitionHeaderFields{{"xmlid", "FIRST"}}));
}

TEST(DefinitionHeaderFields, DoesNotSearchArbitraryWrappersOrNestedRoms)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    for (const std::string source : {"<wrapper><rom><romid><xmlid>ID</xmlid></romid></rom></wrapper>",
                                     "<roms><wrapper><rom><romid><xmlid>ID</xmlid></romid></rom></wrapper></roms>",
                                     "<wrapper><romid><xmlid>ID</xmlid></romid></wrapper>", "<roms/>"})
    {
        SCOPED_TRACE(source);
        EXPECT_EQ(collect_ecuflash_base_header_fields(names, source), (DefinitionHeaderFields{{"xmlid", ""}}));
    }
}

TEST(DefinitionHeaderFields, MapsEveryFieldAndKeepsLastValueForDuplicateNames)
{
    const DefinitionHeaderFields fields{{"xmlid", "OLD"},
                                        {"xmlid", " ID "},
                                        {"internalidaddress", "2f8000"},
                                        {"internalidstring", " internal "},
                                        {"ecuid", " ECU "},
                                        {"make", "make"},
                                        {"market", "market"},
                                        {"model", "model"},
                                        {"submodel", "submodel"},
                                        {"transmission", "transmission"},
                                        {"year", "year"},
                                        {"flashmethod", "flash"},
                                        {"memmodel", "memory"},
                                        {"checksummodule", "checksum"},
                                        {"include", "parent"},
                                        {"notes", "note body"},
                                        {"unknown", "ignored"}};
    const auto input = definition_header_input(fields);
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->xml_id, "ID");
    EXPECT_EQ(input->internal_id, " internal ");
    EXPECT_EQ(input->ecu_id, " ECU ");
    EXPECT_EQ(input->internal_id_address, 0x2f8000U);
    EXPECT_EQ(input->metadata.make, "make");
    EXPECT_EQ(input->metadata.market, "market");
    EXPECT_EQ(input->metadata.model, "model");
    EXPECT_EQ(input->metadata.submodel, "submodel");
    EXPECT_EQ(input->metadata.transmission, "transmission");
    EXPECT_EQ(input->metadata.year, "year");
    EXPECT_EQ(input->metadata.flash_method, "flash");
    EXPECT_EQ(input->metadata.memory_model, "memory");
    EXPECT_EQ(input->metadata.checksum_module, "checksum");
    EXPECT_EQ(input->include, "parent");
    EXPECT_EQ(input->notes, "note body");
}

TEST(DefinitionHeaderFields, MissingFieldsAndBlankAddressRemainOptional)
{
    const auto input = definition_header_input(DefinitionHeaderFields{{"internalidaddress", " \t\r\n\v\f "}});
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->xml_id, "");
    EXPECT_EQ(input->internal_id, "");
    EXPECT_EQ(input->ecu_id, "");
    EXPECT_EQ(input->internal_id_address, std::nullopt);
}

TEST(DefinitionHeaderFields, AcceptsHexPrefixesPlusAndUint64Maximum)
{
    for (const std::string address : {"10", "0X10", "+0x10", "ffffffffffffffff"})
    {
        SCOPED_TRACE(address);
        const auto input = definition_header_input(DefinitionHeaderFields{{"internalidaddress", address}});
        ASSERT_THAT(input, IsOk());
        EXPECT_EQ(input->internal_id_address,
                  address == "ffffffffffffffff" ? std::numeric_limits<std::uint64_t>::max() : 16U);
    }
}

TEST(DefinitionHeaderFields, RejectsInvalidTrailingJunkNegativeAndOverflowingAddresses)
{
    for (const std::string address : {"not-hex", "0x", "+", "+ 10", "-0", "10junk", "10000000000000000"})
    {
        SCOPED_TRACE(address);
        EXPECT_THAT(
            definition_header_input(DefinitionHeaderFields{{"internalidaddress", address}}),
            IsErrWith(fastecu::ErrorKind::InvalidConfig, "definition internal ID address is not a valid integer"));
    }
}

TEST(DefinitionHeaderFields, TrimsUnicodeWhitespaceOnlyOnXmlIdAndAddress)
{
    const auto input = definition_header_input(DefinitionHeaderFields{{"xmlid", "\xe2\x80\x83ID\xc2\xa0"},
                                                                      {"internalidaddress", "\xc2\xa0"
                                                                                            "10\xe3\x80\x80"},
                                                                      {"ecuid", " ECU "},
                                                                      {"notes", " notes "}});
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->xml_id, "ID");
    EXPECT_EQ(input->internal_id_address, 16U);
    EXPECT_EQ(input->ecu_id, " ECU ");
    EXPECT_EQ(input->notes, " notes ");
}
} // namespace

TEST(DefinitionHeaderFields, RejectsMultipleDocumentRootsAndKeepsUtf8DespiteEncodingDeclaration)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(names, "<rom><romid><xmlid>first</xmlid></romid></rom><rom/>"),
              (DefinitionHeaderFields{{"xmlid", ""}}));
    EXPECT_EQ(
        collect_ecuflash_base_header_fields(
            names,
            "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>"),
        (DefinitionHeaderFields{{"xmlid", "Caf\xc3\xa9"}}));
}

TEST(DefinitionHeaderFields, LeavesCommentAndCdataEntitySpellingsLiteral)
{
    const auto names = std::to_array<std::string>({"notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<rom><!-- &undefined; --><notes><![CDATA[A&undefined;B]]></notes></rom>"),
              (DefinitionHeaderFields{{"notes", "A&undefined;B"}}));
}

TEST(DefinitionHeaderFields, ReadsPredefinedAndNumericReferencesWithoutDtdExpansion)
{
    const auto names = std::to_array<std::string>({"notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<!DOCTYPE rom><rom><notes>&lt; &gt; &amp; &apos; &quot; &#65; &#x42;</notes></rom>"),
              (DefinitionHeaderFields{{"notes", "< > & ' \" A B"}}));
}

TEST(DefinitionHeaderFields, FormMetadataPreservesRawEditableValuesWithoutAcquiringParserExtras)
{
    const DefinitionHeaderFields fields{{"make", " make "},
                                        {"market", " market "},
                                        {"model", " model "},
                                        {"submodel", " submodel "},
                                        {"transmission", " transmission "},
                                        {"year", " year "},
                                        {"flashmethod", " flash "},
                                        {"memmodel", " memory "},
                                        {"checksummodule", " checksum "},
                                        {"filesize", "1024"},
                                        {"notes", " root notes "}};
    const auto input = definition_header_input(fields);
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->metadata, (fastecu::definition::RomMetadata{.make = " make ",
                                                                 .market = " market ",
                                                                 .model = " model ",
                                                                 .submodel = " submodel ",
                                                                 .transmission = " transmission ",
                                                                 .year = " year ",
                                                                 .flash_method = " flash ",
                                                                 .memory_model = " memory ",
                                                                 .checksum_module = " checksum ",
                                                                 .file_size = "",
                                                                 .notes = ""}));
    EXPECT_EQ(input->notes, " root notes ");
}
