#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/definition/definition_writer.h"

#include <cstdint>
#include <format>
#include <optional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/definition/ecuflash_parser.h"

namespace fastecu::definition
{
namespace
{

using ::testing::HasSubstr;
using ::testing::Not;

std::vector<std::uint8_t> Bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

std::string Text(std::span<const std::uint8_t> value)
{
    return {value.begin(), value.end()};
}

DefinitionHeaderInput CompleteInput()
{
    return DefinitionHeaderInput{
        .xml_id = "NEW_XML",
        .internal_id = "A1B2C3",
        .ecu_id = "ECU-42",
        .internal_id_address = 0x1A0,
        .metadata =
            RomMetadata{
                .make = "Subaru",
                .market = "EU",
                .model = "Legacy",
                .submodel = "GT",
                .transmission = "6MT",
                .year = "2008",
                .flash_method = "subaru_denso_can",
                .memory_model = "SH7058",
                .checksum_module = "subarudbw",
                .file_size = "1048576",
                .notes = "Identity notes",
            },
        .include = "BASE_XML",
        .notes = "Réglage Ω",
    };
}

TEST(DefinitionWriterTest, CreatesSemanticEcuFlashDefinitionWithDeterministicUtf8Layout)
{
    const DefinitionHeaderInput input = CompleteInput();

    auto result = CreateEcuflashXml(input);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    auto parsed = ParseEcuflashDefinition(*result, "created.xml");
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->identity, (RomIdentity{
                                    .xml_id = input.xml_id,
                                    .internal_id = input.internal_id,
                                    .ecu_id = input.ecu_id,
                                    .internal_id_address = input.internal_id_address,
                                }));
    EXPECT_EQ(parsed->metadata, input.metadata);
    EXPECT_EQ(parsed->parents, std::vector<std::string>{input.include});

    const std::string xml = Text(*result);
    EXPECT_TRUE(xml.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"));
    EXPECT_THAT(xml, HasSubstr("\n  <romid>\n    <xmlid>NEW_XML</xmlid>"));
    EXPECT_THAT(xml, HasSubstr("<notes>Réglage Ω</notes>"));
    EXPECT_THAT(xml, Not(HasSubstr("\t")));
    ASSERT_FALSE(xml.empty());
    EXPECT_EQ(xml.back(), '\n');
}

TEST(DefinitionWriterTest, OmitsAddressElementWhenNotProvided)
{
    DefinitionHeaderInput input = CompleteInput();
    input.internal_id_address = std::nullopt;

    auto result = CreateEcuflashXml(input);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    auto parsed = ParseEcuflashDefinition(*result, "created.xml");
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->identity.internal_id_address, std::nullopt);

    const std::string xml = Text(*result);
    EXPECT_THAT(xml, Not(HasSubstr("internalidaddress")));
}

TEST(DefinitionWriterTest, ClearsExistingAddressOnRewriteWhenNotProvided)
{
    const auto source = Bytes(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<rom>
  <romid>
    <xmlid>OLD_XML</xmlid>
    <internalidaddress>0x1a0</internalidaddress>
    <internalidstring>OLD_ID</internalidstring>
    <ecuid>OLD_ECU</ecuid>
  </romid>
</rom>
)xml");
    DefinitionHeaderInput input = CompleteInput();
    input.internal_id_address = std::nullopt;

    auto result = RewriteEcuflashXml(source, input);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    auto parsed = ParseEcuflashDefinition(*result, "rewritten.xml");
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->identity.internal_id_address, std::nullopt);

    const std::string xml = Text(*result);
    EXPECT_THAT(xml, Not(HasSubstr("internalidaddress")));
}

TEST(DefinitionWriterTest, ReplacesStaleNestedContentInsteadOfAppendingToIt)
{
    const auto source = Bytes(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<rom>
  <romid>
    <xmlid><stale attr="1">junk</stale></xmlid>
    <internalidstring>OLD_ID</internalidstring>
    <ecuid>OLD_ECU</ecuid>
  </romid>
</rom>
)xml");
    const DefinitionHeaderInput input = CompleteInput();

    auto result = RewriteEcuflashXml(source, input);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    auto parsed = ParseEcuflashDefinition(*result, "rewritten.xml");
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->identity.xml_id, input.xml_id);

    const std::string xml = Text(*result);
    EXPECT_THAT(xml, Not(HasSubstr("stale")));
    EXPECT_THAT(xml, Not(HasSubstr("junk")));
    EXPECT_THAT(xml, HasSubstr(std::format("<xmlid>{}</xmlid>", input.xml_id)));
}

TEST(DefinitionWriterTest, RejectsEachEmptyRequiredIdentity)
{
    for (const std::string_view field : {"xml_id", "internal_id", "ecu_id"})
    {
        DefinitionHeaderInput input = CompleteInput();
        if (field == "xml_id")
        {
            input.xml_id.clear();
        }
        else if (field == "internal_id")
        {
            input.internal_id.clear();
        }
        else
        {
            input.ecu_id.clear();
        }

        ASSERT_THAT(CreateEcuflashXml(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << field;
    }
}

TEST(DefinitionWriterTest, RewritesHeaderAndPreservesUnrelatedTreeContent)
{
    const auto source = Bytes(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<rom custom="keep">
  <!-- root comment -->
  <romid>
    <xmlid>OLD_XML</xmlid>
    <xmlid>STALE_XML</xmlid>
    <internalidaddress>BAD</internalidaddress>
    <internalidstring>OLD_ID</internalidstring>
    <ecuid>OLD_ECU</ecuid>
    <make>Old make</make>
    <year>1999</year>
    <notes>Old identity notes</notes>
    <!-- identity comment -->
    <vendor-field flag="yes">keep me</vendor-field>
  </romid>
  <include>OLD_BASE</include>
  <include>STALE_BASE</include>
  <notes>Old document notes</notes>
  <notes>Stale document notes</notes>
  <scaling name="scale" units="V" expression="x" to_byte="x"/>
  <table name="Fuel" address="100"><table type="X Axis" name="Load" elements="1"/></table>
  <vendor-extension answer="42"><child/></vendor-extension>
</rom>
)xml");
    const DefinitionHeaderInput input = CompleteInput();

    auto result = RewriteEcuflashXml(source, input);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    auto parsed = ParseEcuflashDefinition(*result, "rewritten.xml");
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->identity.xml_id, input.xml_id);
    EXPECT_EQ(parsed->identity.internal_id, input.internal_id);
    EXPECT_EQ(parsed->identity.ecu_id, input.ecu_id);
    EXPECT_EQ(parsed->identity.internal_id_address, input.internal_id_address);
    EXPECT_EQ(parsed->metadata, input.metadata);
    EXPECT_EQ(parsed->parents, std::vector<std::string>{input.include});
    ASSERT_EQ(parsed->scalings.size(), 1U);
    EXPECT_EQ(parsed->scalings.front().name, "scale");
    ASSERT_EQ(parsed->maps.size(), 1U);
    EXPECT_EQ(parsed->maps.front().name, "Fuel");

    const std::string xml = Text(*result);
    EXPECT_THAT(xml, HasSubstr("<!-- root comment -->"));
    EXPECT_THAT(xml, HasSubstr("<!-- identity comment -->"));
    EXPECT_THAT(xml, HasSubstr("<vendor-field flag=\"yes\">keep me</vendor-field>"));
    EXPECT_THAT(xml, HasSubstr("<vendor-extension answer=\"42\">\n    <child />\n  </vendor-extension>"));
    EXPECT_THAT(xml, HasSubstr("<notes>Réglage Ω</notes>"));
    EXPECT_THAT(xml, Not(HasSubstr("OLD_XML")));
    EXPECT_THAT(xml, Not(HasSubstr("OLD_BASE")));
    EXPECT_THAT(xml, Not(HasSubstr("Old document notes")));
    EXPECT_THAT(xml, Not(HasSubstr("STALE_XML")));
    EXPECT_THAT(xml, Not(HasSubstr("STALE_BASE")));
    EXPECT_THAT(xml, Not(HasSubstr("Stale document notes")));
    EXPECT_THAT(xml, Not(HasSubstr("\t")));
    EXPECT_EQ(xml.back(), '\n');
}

TEST(DefinitionWriterTest, RejectsMalformedImportBeforeProducingBytes)
{
    ASSERT_THAT(RewriteEcuflashXml(Bytes("<rom><romid>"), CompleteInput()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DefinitionWriterTest, RejectsDuplicateTopLevelRomIdContainers)
{
    const auto source = Bytes(R"xml(
<rom>
  <romid><xmlid>FIRST</xmlid></romid>
  <romid><xmlid>SECOND</xmlid><vendor-field>keep</vendor-field></romid>
</rom>)xml");

    auto result = RewriteEcuflashXml(source, CompleteInput());

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(result.error().detail, HasSubstr("<romid>"));
    EXPECT_THAT(result.error().detail, HasSubstr("duplicate"));
}

TEST(DefinitionWriterTest, CanonicalizesDirectCreateAndRewriteInputsWithoutChangingNotes)
{
    auto input = CompleteInput();
    input.xml_id = "\xc2\xa0"
                   "CAL123\xe3\x80\x80";
    input.internal_id = " INTERNAL ID ";
    input.ecu_id = " ECU ";
    input.include = " BASE ";
    input.metadata.model = " Mitsubishi Colt ";
    input.metadata.make = "\xe2\x80\x83Mitsubishi\xc2\xa0";
    input.metadata.file_size = " 1024 ";
    input.metadata.notes = " \n identity notes \n ";
    input.notes = " \n document notes \n ";
    input.internal_id_address = std::numeric_limits<std::uint64_t>::max();
    const auto created = CreateEcuflashXml(input);
    const auto rewritten =
        RewriteEcuflashXml(Bytes("<rom><romid><xmlid>OLD</xmlid></romid><!-- keep --></rom>"), input);
    for (const auto *result : {&created, &rewritten})
    {
        ASSERT_THAT(*result, fastecu::testing::IsOk());
        const auto xml = Text(**result);
        EXPECT_THAT(xml, HasSubstr("<xmlid>CAL123</xmlid>"));
        EXPECT_THAT(xml, HasSubstr("<internalidstring>INTERNAL ID</internalidstring>"));
        EXPECT_THAT(xml, HasSubstr("<ecuid>ECU</ecuid>"));
        EXPECT_THAT(xml, HasSubstr("<include>BASE</include>"));
        EXPECT_THAT(xml, HasSubstr("<model>Mitsubishi Colt</model>"));
        EXPECT_THAT(xml, HasSubstr("<make>Mitsubishi</make>"));
        EXPECT_THAT(xml, HasSubstr("<filesize>1024</filesize>"));
        EXPECT_THAT(xml, HasSubstr("<internalidaddress>0xffffffffffffffff</internalidaddress>"));
        EXPECT_THAT(xml, HasSubstr("<notes> \n identity notes \n </notes>"));
        EXPECT_THAT(xml, HasSubstr("<notes> \n document notes \n </notes>"));
        const auto parsed = ParseEcuflashDefinition(**result, "canonical.xml");
        ASSERT_THAT(parsed, fastecu::testing::IsOk());
        EXPECT_EQ(parsed->identity.xml_id, "CAL123");
        EXPECT_EQ(parsed->identity.internal_id, "INTERNAL ID");
        EXPECT_EQ(parsed->identity.internal_id_address, std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(parsed->metadata.notes, " \n identity notes \n ");
    }
    ASSERT_THAT(rewritten, fastecu::testing::IsOk());
    EXPECT_THAT(Text(*rewritten), HasSubstr("<!-- keep -->"));
    ASSERT_THAT(created, fastecu::testing::IsOk());
    const auto second_write = RewriteEcuflashXml(*created, input);
    ASSERT_THAT(second_write, fastecu::testing::IsOk());
    EXPECT_EQ(*second_write, *created);
    EXPECT_EQ(input.ecu_id, " ECU ");
}

TEST(DefinitionWriterTest, RejectsUnicodeWhitespaceOnlyRequiredFieldsForEveryCaller)
{
    for (const auto member :
         {&DefinitionHeaderInput::xml_id, &DefinitionHeaderInput::internal_id, &DefinitionHeaderInput::ecu_id})
    {
        auto input = CompleteInput();
        input.*member = "\xc2\xa0\xe2\x80\x83\xe3\x80\x80";
        EXPECT_THAT(CreateEcuflashXml(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        EXPECT_THAT(RewriteEcuflashXml(Bytes("<rom/>"), input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

} // namespace
} // namespace fastecu::definition
