#include "src/backend/definition/definition_writer.h"

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/definition/ecuflash_parser.h"

namespace fastecu::definition
{
namespace
{

std::vector<std::uint8_t> bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

std::string text(std::span<const std::uint8_t> value)
{
    return {value.begin(), value.end()};
}

DefinitionHeaderInput complete_input()
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
    const DefinitionHeaderInput input = complete_input();

    auto result = create_ecuflash_xml(input);

    ASSERT_TRUE(result) << result.error().detail;
    auto parsed = parse_ecuflash_definition(*result, "created.xml");
    ASSERT_TRUE(parsed) << parsed.error().detail;
    EXPECT_EQ(
        parsed->identity,
        (RomIdentity{
            .xml_id = input.xml_id,
            .internal_id = input.internal_id,
            .ecu_id = input.ecu_id,
            .internal_id_address = input.internal_id_address,
        }));
    EXPECT_EQ(parsed->metadata, input.metadata);
    EXPECT_EQ(parsed->parents, std::vector<std::string>{input.include});

    const std::string xml = text(*result);
    EXPECT_TRUE(xml.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"));
    EXPECT_NE(xml.find("\n  <romid>\n    <xmlid>NEW_XML</xmlid>"), std::string::npos);
    EXPECT_NE(xml.find("<notes>Réglage Ω</notes>"), std::string::npos);
    EXPECT_EQ(xml.find('\t'), std::string::npos);
    ASSERT_FALSE(xml.empty());
    EXPECT_EQ(xml.back(), '\n');
}

TEST(DefinitionWriterTest, OmitsAddressElementWhenNotProvided)
{
    DefinitionHeaderInput input = complete_input();
    input.internal_id_address = std::nullopt;

    auto result = create_ecuflash_xml(input);

    ASSERT_TRUE(result) << result.error().detail;
    auto parsed = parse_ecuflash_definition(*result, "created.xml");
    ASSERT_TRUE(parsed) << parsed.error().detail;
    EXPECT_EQ(parsed->identity.internal_id_address, std::nullopt);

    const std::string xml = text(*result);
    EXPECT_EQ(xml.find("internalidaddress"), std::string::npos);
}

TEST(DefinitionWriterTest, ClearsExistingAddressOnRewriteWhenNotProvided)
{
    const auto source = bytes(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<rom>
  <romid>
    <xmlid>OLD_XML</xmlid>
    <internalidaddress>0x1a0</internalidaddress>
    <internalidstring>OLD_ID</internalidstring>
    <ecuid>OLD_ECU</ecuid>
  </romid>
</rom>
)xml");
    DefinitionHeaderInput input = complete_input();
    input.internal_id_address = std::nullopt;

    auto result = rewrite_ecuflash_xml(source, input);

    ASSERT_TRUE(result) << result.error().detail;
    auto parsed = parse_ecuflash_definition(*result, "rewritten.xml");
    ASSERT_TRUE(parsed) << parsed.error().detail;
    EXPECT_EQ(parsed->identity.internal_id_address, std::nullopt);

    const std::string xml = text(*result);
    EXPECT_EQ(xml.find("internalidaddress"), std::string::npos);
}

TEST(DefinitionWriterTest, ReplacesStaleNestedContentInsteadOfAppendingToIt)
{
    const auto source = bytes(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<rom>
  <romid>
    <xmlid><stale attr="1">junk</stale></xmlid>
    <internalidstring>OLD_ID</internalidstring>
    <ecuid>OLD_ECU</ecuid>
  </romid>
</rom>
)xml");
    const DefinitionHeaderInput input = complete_input();

    auto result = rewrite_ecuflash_xml(source, input);

    ASSERT_TRUE(result) << result.error().detail;
    auto parsed = parse_ecuflash_definition(*result, "rewritten.xml");
    ASSERT_TRUE(parsed) << parsed.error().detail;
    EXPECT_EQ(parsed->identity.xml_id, input.xml_id);

    const std::string xml = text(*result);
    EXPECT_EQ(xml.find("stale"), std::string::npos);
    EXPECT_EQ(xml.find("junk"), std::string::npos);
    EXPECT_NE(xml.find(std::format("<xmlid>{}</xmlid>", input.xml_id)), std::string::npos);
}

TEST(DefinitionWriterTest, RejectsEachEmptyRequiredIdentity)
{
    for (const std::string_view field : {"xml_id", "internal_id", "ecu_id"})
    {
        DefinitionHeaderInput input = complete_input();
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

        auto result = create_ecuflash_xml(input);

        ASSERT_FALSE(result) << field;
        EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig) << field;
    }
}

TEST(DefinitionWriterTest, RewritesHeaderAndPreservesUnrelatedTreeContent)
{
    const auto source = bytes(R"xml(<?xml version="1.0" encoding="UTF-8"?>
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
    const DefinitionHeaderInput input = complete_input();

    auto result = rewrite_ecuflash_xml(source, input);

    ASSERT_TRUE(result) << result.error().detail;
    auto parsed = parse_ecuflash_definition(*result, "rewritten.xml");
    ASSERT_TRUE(parsed) << parsed.error().detail;
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

    const std::string xml = text(*result);
    EXPECT_NE(xml.find("<!-- root comment -->"), std::string::npos);
    EXPECT_NE(xml.find("<!-- identity comment -->"), std::string::npos);
    EXPECT_NE(
        xml.find("<vendor-field flag=\"yes\">keep me</vendor-field>"),
        std::string::npos);
    EXPECT_NE(
        xml.find("<vendor-extension answer=\"42\">\n    <child />\n  </vendor-extension>"),
        std::string::npos);
    EXPECT_NE(xml.find("<notes>Réglage Ω</notes>"), std::string::npos);
    EXPECT_EQ(xml.find("OLD_XML"), std::string::npos);
    EXPECT_EQ(xml.find("OLD_BASE"), std::string::npos);
    EXPECT_EQ(xml.find("Old document notes"), std::string::npos);
    EXPECT_EQ(xml.find("STALE_XML"), std::string::npos);
    EXPECT_EQ(xml.find("STALE_BASE"), std::string::npos);
    EXPECT_EQ(xml.find("Stale document notes"), std::string::npos);
    EXPECT_EQ(xml.find('\t'), std::string::npos);
    EXPECT_EQ(xml.back(), '\n');
}

TEST(DefinitionWriterTest, RejectsMalformedImportBeforeProducingBytes)
{
    auto result = rewrite_ecuflash_xml(bytes("<rom><romid>"), complete_input());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionWriterTest, RejectsDuplicateTopLevelRomIdContainers)
{
    const auto source = bytes(R"xml(
<rom>
  <romid><xmlid>FIRST</xmlid></romid>
  <romid><xmlid>SECOND</xmlid><vendor-field>keep</vendor-field></romid>
</rom>)xml");

    auto result = rewrite_ecuflash_xml(source, complete_input());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("<romid>"), std::string::npos);
    EXPECT_NE(result.error().detail.find("duplicate"), std::string::npos);
}

} // namespace
} // namespace fastecu::definition
