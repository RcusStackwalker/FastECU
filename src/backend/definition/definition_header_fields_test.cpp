// Header parsing migrated from src/ui/desktop/definition/definition_header_form.cpp.
#include "src/backend/definition/definition_header_fields.h"

#include <array>
#include <limits>
#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/romraider_parser.h"

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using fastecu::definition::collect_ecuflash_base_header_fields;
using fastecu::definition::definition_header_input;
using fastecu::definition::DefinitionHeaderFields;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;

TEST(DefinitionHeaderFields, PreservesRequestedOrderAndAbsentFields)
{
    const auto names = std::to_array<std::string>({"xmlid", "model", "internalidaddress", "include", "notes"});
    EXPECT_THAT(
        collect_ecuflash_base_header_fields(
            names, "<rom><romid><xmlid>  BASE  </xmlid><internalidaddress>0x2000</internalidaddress></romid></rom>"),
        IsOkAnd(::testing::Eq((DefinitionHeaderFields{
            {"xmlid", "BASE"}, {"model", ""}, {"internalidaddress", "0x2000"}, {"include", ""}, {"notes", ""}}))));
}

TEST(DefinitionHeaderFields, ReadsWrappedRomAndConcatenatesDirectText)
{
    const auto names = std::to_array<std::string>({"xmlid", "include", "notes"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(
                    names, "<roms><rom><romid><xmlid>BASE</xmlid></romid><include>OEM_BASE</include>"
                           "<notes>Text &amp; <!-- split -->direct<![CDATA[ notes]]></notes></rom></roms>"),
                IsOkAnd(::testing::Eq((DefinitionHeaderFields{
                    {"xmlid", "BASE"}, {"include", "OEM_BASE"}, {"notes", "Text & direct notes"}}))));
}

TEST(DefinitionHeaderFields, MalformedXmlReportsAnError)
{
    const auto names = std::to_array<std::string>({"xmlid", "include", "notes"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(names, "<rom><romid>"), IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(DefinitionHeaderFields, SelectsFirstDirectRomInRomsContainer)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(
                    names, "<roms><!-- comment --><metadata/><rom><romid><xmlid>FIRST</xmlid></romid></rom>"
                           "<rom><romid><xmlid>SECOND</xmlid></romid></rom></roms>"),
                IsOkAnd(::testing::Eq((DefinitionHeaderFields{{"xmlid", "FIRST"}}))));
}

TEST(DefinitionHeaderFields, DoesNotSearchArbitraryWrappersOrNestedRoms)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    for (const std::string source : {"<wrapper><rom><romid><xmlid>ID</xmlid></romid></rom></wrapper>",
                                     "<roms><wrapper><rom><romid><xmlid>ID</xmlid></romid></rom></wrapper></roms>",
                                     "<wrapper><romid><xmlid>ID</xmlid></romid></wrapper>", "<roms/>"})
    {
        SCOPED_TRACE(source);
        EXPECT_THAT(collect_ecuflash_base_header_fields(names, source), IsErr(fastecu::ErrorKind::InvalidConfig));
    }
}

TEST(DefinitionHeaderFields, MapsEveryEditableField)
{
    const DefinitionHeaderFields fields{{"xmlid", " ID "},
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
    EXPECT_EQ(input->internal_id, "internal");
    EXPECT_EQ(input->ecu_id, "ECU");
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
        EXPECT_THAT(definition_header_input(DefinitionHeaderFields{{"internalidaddress", address}}),
                    fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
    }
}

TEST(DefinitionHeaderFields, NormalizesScalarFieldsAndPreservesNotes)
{
    const auto input = definition_header_input(DefinitionHeaderFields{{"xmlid", "\xe2\x80\x83ID\xc2\xa0"},
                                                                      {"internalidaddress", "\xc2\xa0"
                                                                                            "10\xe3\x80\x80"},
                                                                      {"ecuid", " ECU "},
                                                                      {"notes", " notes "}});
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->xml_id, "ID");
    EXPECT_EQ(input->internal_id_address, 16U);
    EXPECT_EQ(input->ecu_id, "ECU");
    EXPECT_EQ(input->notes, " notes ");
}
} // namespace

TEST(DefinitionHeaderFields, RejectsMultipleDocumentRootsAndKeepsUtf8DespiteEncodingDeclaration)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(names, "<rom><romid><xmlid>first</xmlid></romid></rom><rom/>"),
                IsErr(fastecu::ErrorKind::InvalidConfig));
    EXPECT_THAT(
        collect_ecuflash_base_header_fields(
            names,
            "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>"),
        IsOkAnd(::testing::Eq((DefinitionHeaderFields{{"xmlid", "Caf\xc3\xa9"}}))));
}

TEST(DefinitionHeaderFields, LeavesCommentAndCdataEntitySpellingsLiteral)
{
    const auto names = std::to_array<std::string>({"notes"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(
                    names, "<rom><!-- &undefined; --><notes><![CDATA[A&undefined;B]]></notes></rom>"),
                IsOkAnd(::testing::Eq((DefinitionHeaderFields{{"notes", "A&undefined;B"}}))));
}

TEST(DefinitionHeaderFields, ReadsPredefinedAndNumericReferencesWithoutDtdExpansion)
{
    const auto names = std::to_array<std::string>({"notes"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(
                    names, "<!DOCTYPE rom><rom><notes>&lt; &gt; &amp; &apos; &quot; &#65; &#x42;</notes></rom>"),
                IsOkAnd(::testing::Eq((DefinitionHeaderFields{{"notes", "< > & ' \" A B"}}))));
}

TEST(DefinitionHeaderFields, FormMetadataNormalizesEditableValuesWithoutAcquiringParserExtras)
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
    EXPECT_EQ(input->metadata, (fastecu::definition::RomMetadata{.make = "make",
                                                                 .market = "market",
                                                                 .model = "model",
                                                                 .submodel = "submodel",
                                                                 .transmission = "transmission",
                                                                 .year = "year",
                                                                 .flash_method = "flash",
                                                                 .memory_model = "memory",
                                                                 .checksum_module = "checksum",
                                                                 .file_size = "",
                                                                 .notes = ""}));
    EXPECT_EQ(input->notes, " root notes ");
}

TEST(DefinitionHeaderFields, ImportAndBothLoadersAgreeOnNormalizedHeaderValues)
{
    const std::string rom = "<rom base=\"\xc2\xa0"
                            "BASE\xc2\xa0\"><romid><xmlid> ID </xmlid>"
                            "<internalidstring> INTERNAL </internalidstring><ecuid> ECU </ecuid>"
                            "<internalidaddress> +0X10 </internalidaddress><model> Mitsubishi Colt </model>"
                            "</romid><include>\xc2\xa0"
                            "BASE\xc2\xa0</include></rom>";
    const auto names =
        std::to_array<std::string>({"xmlid", "internalidstring", "ecuid", "internalidaddress", "model", "include"});
    const auto fields = collect_ecuflash_base_header_fields(names, rom);
    ASSERT_THAT(fields, IsOk());
    const auto input = definition_header_input(*fields);
    ASSERT_THAT(input, IsOk());
    const std::vector<std::uint8_t> ecuflash_bytes(rom.begin(), rom.end());
    const auto ecuflash = fastecu::definition::parse_ecuflash_definition(ecuflash_bytes, "header.xml");
    ASSERT_THAT(ecuflash, IsOk());
    const std::string wrapped = "<roms>" + rom + "</roms>";
    const std::vector<std::uint8_t> romraider_bytes(wrapped.begin(), wrapped.end());
    const auto romraider = fastecu::definition::parse_romraider_definition(romraider_bytes, "header.xml", "ID");
    ASSERT_THAT(romraider, IsOk());
    for (const auto *definition : {&*ecuflash, &*romraider})
    {
        EXPECT_EQ(definition->identity.xml_id, input->xml_id);
        EXPECT_EQ(definition->identity.internal_id, input->internal_id);
        EXPECT_EQ(definition->identity.ecu_id, input->ecu_id);
        EXPECT_EQ(definition->identity.internal_id_address, input->internal_id_address);
        EXPECT_EQ(definition->metadata.model, "Mitsubishi Colt");
        EXPECT_EQ(definition->parents, (std::vector<std::string>{"BASE"}));
    }
}

TEST(DefinitionHeaderFields, PartialHeaderDoesNotRequireIdentityOrParseTables)
{
    const auto names = std::to_array<std::string>({"xmlid", "model", "notes"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(names, "<rom><romid><model> Colt </model></romid><table/></rom>"),
                IsOkAnd(::testing::Eq(DefinitionHeaderFields{{"xmlid", ""}, {"model", "Colt"}, {"notes", ""}})));
}

TEST(DefinitionHeaderFields, RejectsNestedScalarContentInsteadOfFlatteningIt)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_THAT(collect_ecuflash_base_header_fields(names, "<rom><romid><xmlid>A<b>B</b></xmlid></romid></rom>"),
                IsErr(fastecu::ErrorKind::InvalidConfig));
}
