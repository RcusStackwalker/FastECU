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

TEST(DefinitionHeaderFields, RetainsFirstChildTraversalAndFiveLevelLimit)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<a><b><c><d><e><rom><romid><xmlid>ID</xmlid></romid></rom></e></d></c></b></a>"),
              (DefinitionHeaderFields{{"xmlid", "ID"}}));
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<a><b><c><d><e><f><rom><romid><xmlid>ID</xmlid></romid></rom></f></e></d></c></b></a>"),
              (DefinitionHeaderFields{{"xmlid", ""}}));
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

TEST(DefinitionHeaderFields, RejectsUndeclaredEntitiesAndInvalidCharacterReferences)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    for (const std::string reference : {"&bogus;", "&#0;", "&#xD800;", "&#x110000;"})
    {
        SCOPED_TRACE(reference);
        EXPECT_EQ(
            collect_ecuflash_base_header_fields(names, "<rom><romid><xmlid>A" + reference + "B</xmlid></romid></rom>"),
            (DefinitionHeaderFields{{"xmlid", ""}}));
    }
}

TEST(DefinitionHeaderFields, ExpandsDeclaredInternalEntitiesIncludingNestedMarkup)
{
    const auto names = std::to_array<std::string>({"xmlid", "notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<!DOCTYPE rom [<!ENTITY id 'BASE'><!ENTITY name '&id;_NAME'><!ENTITY note '<b>nested</b>'>]>"
                         "<rom><romid><xmlid>&name;</xmlid></romid><notes>&note;</notes></rom>"),
              (DefinitionHeaderFields{{"xmlid", "BASE_NAME"}, {"notes", "nested"}}));
}

TEST(DefinitionHeaderFields, PreservesDtdParameterEntitiesAndFirstDeclaration)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_EQ(
        collect_ecuflash_base_header_fields(
            names,
            "<!DOCTYPE rom [<!ENTITY % declarations \"<!ENTITY id 'FIRST'>\">%declarations;<!ENTITY id 'SECOND'>]>"
            "<rom><romid><xmlid>&id;</xmlid></romid></rom>"),
        (DefinitionHeaderFields{{"xmlid", "FIRST"}}));
}

TEST(DefinitionHeaderFields, DistinguishesNumericMarkupFromPredefinedLiteralText)
{
    const auto names = std::to_array<std::string>({"notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names,
                  "<!DOCTYPE rom [<!ENTITY markup '&#60;b>markup&#60;/b>'><!ENTITY literal '&lt;b>text&lt;/b>'>]>"
                  "<rom><notes>&markup; &literal;</notes></rom>"),
              (DefinitionHeaderFields{{"notes", "markup <b>text</b>"}}));
}

TEST(DefinitionHeaderFields, ExternalEntitiesNeverReadFilesAndAreInvalidInAttributes)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<!DOCTYPE rom [<!ENTITY external SYSTEM "
                         "'file:///unavailable'>]><rom><romid><xmlid>A&external;B</xmlid></romid></rom>"),
              (DefinitionHeaderFields{{"xmlid", "AB"}}));
    EXPECT_EQ(
        collect_ecuflash_base_header_fields(
            names, "<!DOCTYPE rom SYSTEM 'file:///unavailable'><rom><romid><xmlid>A&missing;B</xmlid></romid></rom>"),
        (DefinitionHeaderFields{{"xmlid", "AB"}}));
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<!DOCTYPE rom [<!ENTITY external SYSTEM 'file:///unavailable'>]><rom "
                         "unused='&external;'><romid><xmlid>ID</xmlid></romid></rom>"),
              (DefinitionHeaderFields{{"xmlid", ""}}));
}

TEST(DefinitionHeaderFields, RejectsCyclesAndParameterReferencesInsideInternalDeclarations)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    for (const std::string declaration : {"<!ENTITY id '&id;'>", "<!ENTITY id '&other;'><!ENTITY other '&id;'>",
                                          "<!ENTITY % param 'value'><!ENTITY id '%param;'>"})
    {
        SCOPED_TRACE(declaration);
        EXPECT_EQ(collect_ecuflash_base_header_fields(names, "<!DOCTYPE rom [" + declaration +
                                                                 "]><rom><romid><xmlid>&id;</xmlid></romid></rom>"),
                  (DefinitionHeaderFields{{"xmlid", ""}}));
    }
}

TEST(DefinitionHeaderFields, LimitsEachOuterExpansionToQtNetCharacterBudget)
{
    const auto names = std::to_array<std::string>({"xmlid"});
    for (const std::size_t count : {4100U, 4101U})
    {
        SCOPED_TRACE(count);
        const std::string value(count, 'A');
        EXPECT_EQ(collect_ecuflash_base_header_fields(names, "<!DOCTYPE rom [<!ENTITY id '" + value +
                                                                 "'>]><rom><romid><xmlid>&id;</xmlid></romid></rom>"),
                  (DefinitionHeaderFields{{"xmlid", count == 4100U ? value : ""}}));
    }
    const std::string value(3000, 'A');
    const std::string prefix = "<!DOCTYPE rom [<!ENTITY id '" + value + "'><!ENTITY both '&id;&id;'>]>";
    EXPECT_EQ(collect_ecuflash_base_header_fields(names, prefix + "<rom><romid><xmlid>&id;&id;</xmlid></romid></rom>"),
              (DefinitionHeaderFields{{"xmlid", value + value}}));
    EXPECT_EQ(collect_ecuflash_base_header_fields(names, prefix + "<rom><romid><xmlid>&both;</xmlid></romid></rom>"),
              (DefinitionHeaderFields{{"xmlid", ""}}));
}

TEST(DefinitionHeaderFields, LeavesCommentAndCdataEntitySpellingsLiteral)
{
    const auto names = std::to_array<std::string>({"notes"});
    EXPECT_EQ(collect_ecuflash_base_header_fields(
                  names, "<rom><!-- &undefined; --><notes><![CDATA[A&undefined;B]]></notes></rom>"),
              (DefinitionHeaderFields{{"notes", "A&undefined;B"}}));
}
