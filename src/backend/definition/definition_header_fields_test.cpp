#include "src/backend/definition/definition_header_fields.h"

#include <limits>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/romraider_parser.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using fastecu::definition::definition_header_input;
using fastecu::definition::DefinitionHeaderDraft;
using fastecu::definition::read_definition_header;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

TEST(DefinitionHeaderFields, ReadsNamedValuesAndDefaultsAbsentFields)
{
    const auto draft = read_definition_header("<rom><romid><xmlid> BASE </xmlid>"
                                              "<internalidaddress>0x2000</internalidaddress></romid></rom>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->xml_id, "BASE");
    EXPECT_EQ(draft->internal_id_address_text, "0x2000");
    EXPECT_EQ(draft->metadata.model, "");
    EXPECT_EQ(draft->include, "");
    EXPECT_EQ(draft->notes, "");
}

TEST(DefinitionHeaderFields, ReadsWrappedRomAndConcatenatesDirectText)
{
    const auto draft =
        read_definition_header("<roms><rom><romid><xmlid>BASE</xmlid></romid><include>OEM_BASE</include>"
                               "<notes>Text &amp; <!-- split -->direct<![CDATA[ notes]]></notes></rom></roms>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->xml_id, "BASE");
    EXPECT_EQ(draft->include, "OEM_BASE");
    EXPECT_EQ(draft->notes, "Text & direct notes");
}

TEST(DefinitionHeaderFields, MalformedXmlReportsAnError)
{
    EXPECT_THAT(read_definition_header("<rom><romid>"), IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(DefinitionHeaderFields, SelectsFirstDirectRomInRomsContainer)
{
    const auto draft =
        read_definition_header("<roms><!-- comment --><metadata/><rom><romid><xmlid>FIRST</xmlid></romid></rom>"
                               "<rom><romid><xmlid>SECOND</xmlid></romid></rom></roms>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->xml_id, "FIRST");
}

TEST(DefinitionHeaderFields, DoesNotSearchArbitraryWrappersOrNestedRoms)
{
    for (const std::string source : {"<wrapper><rom><romid><xmlid>ID</xmlid></romid></rom></wrapper>",
                                     "<roms><wrapper><rom><romid><xmlid>ID</xmlid></romid></rom></wrapper></roms>",
                                     "<wrapper><romid><xmlid>ID</xmlid></romid></wrapper>", "<roms/>"})
    {
        SCOPED_TRACE(source);
        EXPECT_THAT(read_definition_header(source), IsErr(fastecu::ErrorKind::InvalidConfig));
    }
}

TEST(DefinitionHeaderFields, MapsEveryEditableField)
{
    const auto draft = read_definition_header(
        "<rom><romid><xmlid> ID </xmlid><internalidaddress>2f8000</internalidaddress>"
        "<internalidstring> internal </internalidstring><ecuid> ECU </ecuid><make>make</make><market>market</market>"
        "<model>model</model><submodel>submodel</submodel><transmission>transmission</transmission><year>year</year>"
        "<flashmethod>flash</flashmethod><memmodel>memory</memmodel><checksummodule>checksum</checksummodule>"
        "</romid><include>parent</include><notes>note body</notes></rom>");
    ASSERT_THAT(draft, IsOk());
    const auto input = definition_header_input(*draft);
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
    const auto input = definition_header_input(DefinitionHeaderDraft{.internal_id_address_text = " \t\r\n\v\f "});
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
        const auto input = definition_header_input(DefinitionHeaderDraft{.internal_id_address_text = address});
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
        EXPECT_THAT(definition_header_input(DefinitionHeaderDraft{.internal_id_address_text = address}),
                    IsErr(fastecu::ErrorKind::InvalidConfig));
    }
}

TEST(DefinitionHeaderFields, NormalizesScalarFieldsAndPreservesNotes)
{
    const auto input = definition_header_input(DefinitionHeaderDraft{.xml_id = "\xe2\x80\x83ID\xc2\xa0",
                                                                     .ecu_id = " ECU ",
                                                                     .internal_id_address_text = "\xc2\xa0"
                                                                                                 "10\xe3\x80\x80",
                                                                     .notes = " notes "});
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->xml_id, "ID");
    EXPECT_EQ(input->internal_id_address, 16U);
    EXPECT_EQ(input->ecu_id, "ECU");
    EXPECT_EQ(input->notes, " notes ");
}

TEST(DefinitionHeaderFields, RejectsMultipleRootsAndKeepsDecodedTextUtf8DespiteDeclaration)
{
    EXPECT_THAT(read_definition_header("<rom/><rom/>"), IsErr(fastecu::ErrorKind::InvalidConfig));
    const auto draft = read_definition_header("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"
                                              "<rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->xml_id, "Caf\xc3\xa9");
}

TEST(DefinitionHeaderFields, EntityReferencesAndCdataKeepTheirTextSemantics)
{
    const auto literal =
        read_definition_header("<rom><!-- &undefined; --><notes><![CDATA[A&undefined;B]]></notes></rom>");
    ASSERT_THAT(literal, IsOk());
    EXPECT_EQ(literal->notes, "A&undefined;B");
    const auto expanded =
        read_definition_header("<!DOCTYPE rom><rom><notes>&lt; &gt; &amp; &apos; &quot; &#65; &#x42;</notes></rom>");
    ASSERT_THAT(expanded, IsOk());
    EXPECT_EQ(expanded->notes, "< > & ' \" A B");
}

TEST(DefinitionHeaderFields, EditableMetadataStaysSeparateFromParserExtras)
{
    const auto draft = read_definition_header("<rom><romid><make> make </make><model> model </model>"
                                              "<filesize>1024</filesize><notes>metadata notes</notes></romid>"
                                              "<notes> root notes </notes></rom>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->metadata.make, "make");
    EXPECT_EQ(draft->metadata.model, "model");
    EXPECT_EQ(draft->metadata.file_size, "");
    EXPECT_EQ(draft->metadata.notes, "");
    EXPECT_EQ(draft->notes, " root notes ");
}

TEST(DefinitionHeaderFields, ImportAndBothLoadersAgreeOnNormalizedHeaderValues)
{
    const std::string rom = "<rom base=\"\xc2\xa0"
                            "BASE\xc2\xa0\"><romid><xmlid> ID </xmlid>"
                            "<internalidstring> INTERNAL </internalidstring><ecuid> ECU </ecuid>"
                            "<internalidaddress> +0X10 </internalidaddress><model> Mitsubishi Colt </model>"
                            "</romid><include>\xc2\xa0"
                            "BASE\xc2\xa0</include></rom>";
    const auto draft = read_definition_header(rom);
    ASSERT_THAT(draft, IsOk());
    const auto input = definition_header_input(*draft);
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
    const auto draft = read_definition_header("<rom><romid><model> Colt </model></romid><table/></rom>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->xml_id, "");
    EXPECT_EQ(draft->metadata.model, "Colt");
}

TEST(DefinitionHeaderFields, InvalidAddressTextRemainsEditableUntilSubmission)
{
    const auto draft = read_definition_header("<rom><romid><internalidaddress>+0x</internalidaddress></romid></rom>");
    ASSERT_THAT(draft, IsOk());
    EXPECT_EQ(draft->internal_id_address_text, "+0x");
    EXPECT_THAT(definition_header_input(*draft), IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(DefinitionHeaderFields, RejectsNestedScalarContentInsteadOfFlatteningIt)
{
    EXPECT_THAT(read_definition_header("<rom><romid><xmlid>A<b>B</b></xmlid></romid></rom>"),
                IsErr(fastecu::ErrorKind::InvalidConfig));
}
} // namespace
