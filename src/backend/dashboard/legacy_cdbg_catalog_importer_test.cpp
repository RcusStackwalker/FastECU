#include "src/backend/dashboard/legacy_cdbg_catalog_importer.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace fastecu;
using namespace fastecu::dashboard;

constexpr std::string_view kValidCatalog = R"(<logger><protocols><protocol id="CDBG"><parameters>
  <parameter id="P1" name="First" desc="First description" length="2" enabled="1">
    <address>0x1234</address><conversions>
      <conversion units="rpm" expr="x*2" format="0" gauge_min="0" gauge_max="8000" gauge_step="500"/>
    </conversions>
  </parameter>
</parameters></protocol></protocols></logger>)";

bytes::ByteView byte_view(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

LegacyCdbgImportDefaults valid_defaults()
{
    return LegacyCdbgImportDefaults{
        .document_name = "Imported Colt",
        .bitrate = 500000,
        .identifier_width = CanIdentifierWidth::Standard,
        .stream_instance = 2,
        .sampling_interval_ms = 25,
        .retry =
            RetryPolicy{
                .poll_timeout_ms = 120,
                .silence_threshold = 4,
                .reconnect_attempts = 5,
                .reconnect_period_ms = 300,
            },
    };
}

std::string replace_once(std::string input, std::string_view before, std::string_view after)
{
    const std::size_t position = input.find(before);
    EXPECT_NE(position, std::string::npos) << before;
    if (position != std::string::npos)
    {
        input.replace(position, before.size(), after);
    }
    return input;
}

void expect_import_error(std::string_view xml, std::string_view path,
                         const LegacyCdbgImportDefaults& defaults = valid_defaults())
{
    const auto result = import_legacy_cdbg_catalog(byte_view(xml), defaults);
    ASSERT_FALSE(result) << xml;
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig) << result.error().detail;
    EXPECT_TRUE(result.error().detail.starts_with(path)) << result.error().detail;
}

std::string read_catalog_fixture()
{
    const char *path = std::getenv("CDBG_CATALOG_PATH");
    EXPECT_NE(path, nullptr);
    if (path == nullptr)
    {
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open()) << path;
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
} // namespace

TEST(LegacyCdbgCatalogImporter, ImportsBundledCatalogInSourceOrderWithExplicitConnectionDefaults)
{
    const std::string xml = read_catalog_fixture();
    const auto result = import_legacy_cdbg_catalog(byte_view(xml), valid_defaults());
    ASSERT_TRUE(result) << result.error().detail;

    EXPECT_EQ(result->metadata, (DocumentMetadata{.format_version = 1, .name = "Imported Colt"}));
    EXPECT_EQ(result->connection, (CdbgConnectionProfile{
                                      .protocol = DashboardProtocol::Cdbg,
                                      .transport = DashboardTransport::RawCan,
                                      .bitrate = 500000,
                                      .identifier_width = CanIdentifierWidth::Standard,
                                      .request_id = 0x630,
                                      .reply_id = 0x631,
                                      .stream_instance = 2,
                                      .sampling_interval_ms = 25,
                                      .retry = RetryPolicy{120, 4, 5, 300},
                                      .preferred_adapter = std::nullopt,
                                  }));
    ASSERT_EQ(result->channels.size(), 4U);
    EXPECT_EQ(result->channels[0],
              (DashboardChannel{
                  .id = "CDBG_ENGINE_RPM",
                  .name = "Engine RPM",
                  .description = "engine_rpm uint16, 1000 rpm / 256",
                  .address = 0x804cfc,
                  .length = 2,
                  .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                  .conversions = {DashboardConversion{"conversion-1", "x*1000/256", "rpm", 0, 0, 8000, 500}},
              }));
    EXPECT_EQ(result->channels[1],
              (DashboardChannel{
                  .id = "CDBG_COIL_DWELL_OPTIMAL_RPM_X_POWER",
                  .name = "Coil Dwell Optimal RPM x Power",
                  .description = "coil_dwell_optimal_rpm_x_power raw uint16",
                  .address = 0x804f9c,
                  .length = 2,
                  .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                  .conversions = {DashboardConversion{"conversion-1", "x", "raw", 0, 0, 65535, 1024}},
              }));
    EXPECT_EQ(result->channels[2], (DashboardChannel{
                                       .id = "CDBG_KNOCK_SUM",
                                       .name = "Knock Sum",
                                       .description = "knock_sum raw uint16",
                                       .address = 0x804fbe,
                                       .length = 2,
                                       .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                                       .conversions = {DashboardConversion{"conversion-1", "x", "raw", 0, 0, 65535, 1}},
                                   }));
    EXPECT_EQ(result->channels[3],
              (DashboardChannel{
                  .id = "CDBG_ECU_LOAD",
                  .name = "ECU Load",
                  .description = "ecu_load uint16, 10 percent / 32",
                  .address = 0x804d2a,
                  .length = 2,
                  .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                  .conversions = {DashboardConversion{"conversion-1", "x*10/32", "%", 1, 0, 300, 10}},
              }));
    EXPECT_TRUE(result->cards.empty());
    EXPECT_FALSE(result->connection.preferred_adapter.has_value());
}

TEST(LegacyCdbgCatalogImporter, IgnoresOtherProtocolSubtreesAndCdbgSwitchesButPreservesImportedSourceOrder)
{
    constexpr std::string_view xml = R"(<logger><protocols>
  <protocol id="SSM"><parameters><parameter id="BROKEN"><unknown/></parameter></parameters>
    <protocol id="CDBG"><parameters/></protocol></protocol>
  <protocol id="CDBG"><switches><anything malformed="is ignored"/></switches><parameters>
    <parameter id="SECOND" name="Second" desc="d2" length="1" enabled="0"><address>0x20</address><conversions>
      <conversion units="u1" expr="x" format="0" gauge_min="0" gauge_max="10" gauge_step="1"/>
      <conversion units="u2" expr="x/2" format="0.00" gauge_min="-1" gauge_max="4" gauge_step="0.5"/>
    </conversions></parameter>
    <parameter id="FIRST" name="First" desc="d1" length="4"><address>0x10</address><conversions>
      <conversion units="u" expr="x" format="0.0" gauge_min="0" gauge_max="5" gauge_step="1"/>
    </conversions></parameter>
  </parameters></protocol>
</protocols></logger>)";

    const auto result = import_legacy_cdbg_catalog(byte_view(xml), valid_defaults());
    ASSERT_TRUE(result) << result.error().detail;
    ASSERT_EQ(result->channels.size(), 2U);
    EXPECT_EQ(result->channels[0].id, "SECOND");
    EXPECT_EQ(result->channels[0].address, 0x20U);
    ASSERT_EQ(result->channels[0].conversions.size(), 2U);
    EXPECT_EQ(result->channels[0].conversions[0], (DashboardConversion{"conversion-1", "x", "u1", 0, 0, 10, 1}));
    EXPECT_EQ(result->channels[0].conversions[1], (DashboardConversion{"conversion-2", "x/2", "u2", 2, -1, 4, 0.5}));
    EXPECT_EQ(result->channels[1].id, "FIRST");
    EXPECT_EQ(result->channels[1].length, 4);
}

TEST(LegacyCdbgCatalogImporter, UsesOnlyTheCharacterizedLegacyOptionalDefaults)
{
    constexpr std::string_view xml = R"(<logger><protocols><protocol id="CDBG"><parameters>
  <parameter id="P" name="Name"><address>0x1</address><conversions>
    <conversion gauge_min="0" gauge_max="1" gauge_step="0.1"/>
  </conversions></parameter>
</parameters></protocol></protocols></logger>)";

    const auto result = import_legacy_cdbg_catalog(byte_view(xml), valid_defaults());
    ASSERT_TRUE(result) << result.error().detail;
    ASSERT_EQ(result->channels.size(), 1U);
    EXPECT_EQ(result->channels[0].description, "No desc");
    EXPECT_EQ(result->channels[0].length, 1);
    ASSERT_EQ(result->channels[0].conversions.size(), 1U);
    EXPECT_EQ(result->channels[0].conversions[0], (DashboardConversion{"conversion-1", "x", "#", 2, 0, 1, 0.1}));
}

TEST(LegacyCdbgCatalogImporter, RejectsLexicallyInvalidCompleteInputBeforeImporting)
{
    std::string trailing_nul = std::string(kValidCatalog);
    trailing_nul.push_back('\0');
    trailing_nul.append("<ignored-root/>");

    const std::string malformed_utf8 = "<logger><protocols><protocol id=\"SSM\" note=\"" + std::string("\xc3\x28", 2) +
                                       "\"/><protocol id=\"CDBG\"><parameters>"
                                       "<parameter id=\"P\" name=\"Name\"><address>0x1</address><conversions>"
                                       "<conversion gauge_min=\"0\" gauge_max=\"1\" gauge_step=\"1\"/>"
                                       "</conversions></parameter></parameters></protocol></protocols></logger>";

    const std::string illegal_reference = "<logger><protocols><protocol id=\"SSM\" note=\"&#0;\"/>"
                                          "<protocol id=\"CDBG\"><parameters><parameter id=\"P\" name=\"Name\">"
                                          "<address>0x1</address><conversions>"
                                          "<conversion gauge_min=\"0\" gauge_max=\"1\" gauge_step=\"1\"/>"
                                          "</conversions></parameter></parameters></protocol></protocols></logger>";

    for (const std::string& xml : {trailing_nul, malformed_utf8, illegal_reference})
    {
        expect_import_error(xml, "legacy-cdbg");
    }
}

TEST(LegacyCdbgCatalogImporter, RejectsInvalidNonemptyCallerDefaultStrings)
{
    const std::vector<std::string> invalid_names{
        std::string("Imported\0Colt", 13),
        "Imported " + std::string("\xc3\x28", 2),
        "Imported " + std::string(1, '\x01'),
    };
    for (const std::string& invalid_name : invalid_names)
    {
        auto defaults = valid_defaults();
        defaults.document_name = invalid_name;
        expect_import_error(kValidCatalog, "metadata.name", defaults);
    }
}

TEST(LegacyCdbgCatalogImporter, RejectsTrailingLogicalAddressContent)
{
    const std::string xml = replace_once(std::string(kValidCatalog), "<address>0x1234</address>",
                                         "<address>0x1234<![CDATA[junk]]></address>");
    expect_import_error(xml, "channels[P1].address");
}

TEST(LegacyCdbgCatalogImporter, IgnoresCommentsAndProcessingInstructionsDuringLexicalPreflight)
{
    std::string xml = "<!-- &#0; --><?probe &#0;?>" + std::string(kValidCatalog);
    xml.append("<?probe &#0;?><!-- &#0; -->");
    xml = replace_once(std::move(xml), "<address>0x1234</address>",
                       "<address>0x12<!-- &#0; --><?probe &#0;?>34</address>");
    const auto result = import_legacy_cdbg_catalog(byte_view(xml), valid_defaults());
    ASSERT_TRUE(result) << result.error().detail;
    ASSERT_EQ(result->channels.size(), 1U);
    EXPECT_EQ(result->channels.front().address, 0x1234U);
}

TEST(LegacyCdbgCatalogImporter, RejectsMalformedRootsAndAnAmbiguousCdbgSelectionAtomically)
{
    const std::vector<std::pair<std::string, std::string_view>> cases = {
        {"<logger><protocols>", "legacy-cdbg"},
        {"<config><protocols><protocol id=\"CDBG\"/></protocols></config>", "legacy-cdbg.root"},
        {"<logger/>", "legacy-cdbg.protocols"},
        {"<logger><protocols><protocol id=\"SSM\"/></protocols></logger>", "legacy-cdbg.protocol"},
        {"<logger><protocols><protocol id=\"CDBG\"/><protocol id=\"CDBG\"/></protocols></logger>",
         "legacy-cdbg.protocol"},
    };
    for (const auto& [xml, path] : cases)
    {
        expect_import_error(xml, path);
    }
}

TEST(LegacyCdbgCatalogImporter, RejectsMissingInvalidAndDuplicateParameterFieldsAtomically)
{
    const std::vector<std::pair<std::string, std::string_view>> cases = {
        {"<logger><protocols><protocol id=\"CDBG\"></protocol></protocols></logger>",
         "legacy-cdbg.protocol.parameters"},
        {"<logger><protocols><protocol id=\"CDBG\"><parameters/></protocol></protocols></logger>",
         "legacy-cdbg.protocol.parameters"},
        {replace_once(std::string(kValidCatalog), " id=\"P1\"", ""), "channels[0].id"},
        {replace_once(std::string(kValidCatalog), "id=\"P1\"", "id=\"No id\""), "channels[No id].id"},
        {replace_once(std::string(kValidCatalog), "id=\"P1\"", "id=\"\""), "channels[0].id"},
        {replace_once(std::string(kValidCatalog), "  </parameter>\n",
                      "  </parameter>\n  <parameter id=\"P1\" name=\"Duplicate\" desc=\"d\" length=\"1\">"
                      "<address>0x1</address><conversions><conversion expr=\"x\" format=\"0\" gauge_min=\"0\" "
                      "gauge_max=\"1\" gauge_step=\"1\"/></conversions></parameter>\n"),
         "channels[P1].id"},
        {replace_once(std::string(kValidCatalog), "    <address>0x1234</address>", ""), "channels[P1].address"},
        {replace_once(std::string(kValidCatalog), "0x1234", "0x100000000"), "channels[P1].address"},
        {replace_once(std::string(kValidCatalog), "length=\"2\"", "length=\"3\""), "channels[P1].length"},
        {replace_once(std::string(kValidCatalog),
                      "<conversions>\n      <conversion units=\"rpm\" expr=\"x*2\" format=\"0\" gauge_min=\"0\" "
                      "gauge_max=\"8000\" gauge_step=\"500\"/>\n    </conversions>",
                      ""),
         "channels[P1].conversions"},
        {replace_once(std::string(kValidCatalog),
                      "<conversion units=\"rpm\" expr=\"x*2\" format=\"0\" gauge_min=\"0\" "
                      "gauge_max=\"8000\" gauge_step=\"500\"/>",
                      ""),
         "channels[P1].conversions"},
    };
    for (const auto& [xml, path] : cases)
    {
        expect_import_error(xml, path);
    }
}

TEST(LegacyCdbgCatalogImporter, RejectsInvalidConversionSemanticsAndGaugeFieldsAtomically)
{
    const std::vector<std::pair<std::string, std::string_view>> cases = {
        {replace_once(std::string(kValidCatalog), "expr=\"x*2\"", "expr=\"x trailing\""),
         "channels[P1].conversions[conversion-1].expression"},
        {replace_once(std::string(kValidCatalog), "expr=\"x*2\"", "expr=\"1/(x-x)\""),
         "channels[P1].conversions[conversion-1].expression"},
        {replace_once(std::string(kValidCatalog), "format=\"0\"", "format=\"0.\""),
         "channels[P1].conversions[conversion-1].format"},
        {replace_once(std::string(kValidCatalog), "format=\"0\"", "format=\"1.0\""),
         "channels[P1].conversions[conversion-1].format"},
        {replace_once(std::string(kValidCatalog), " gauge_min=\"0\"", ""),
         "channels[P1].conversions[conversion-1].gauge-min"},
        {replace_once(std::string(kValidCatalog), " gauge_max=\"8000\"", ""),
         "channels[P1].conversions[conversion-1].gauge-max"},
        {replace_once(std::string(kValidCatalog), " gauge_step=\"500\"", ""),
         "channels[P1].conversions[conversion-1].gauge-step"},
        {replace_once(std::string(kValidCatalog), "gauge_min=\"0\"", "gauge_min=\"nan\""),
         "channels[P1].conversions[conversion-1].gauge-min"},
        {replace_once(std::string(kValidCatalog), "gauge_max=\"8000\"", "gauge_max=\"inf\""),
         "channels[P1].conversions[conversion-1].gauge-max"},
        {replace_once(std::string(kValidCatalog), "gauge_step=\"500\"", "gauge_step=\"not-a-number\""),
         "channels[P1].conversions[conversion-1].gauge-step"},
        {replace_once(std::string(kValidCatalog), "gauge_max=\"8000\"", "gauge_max=\"0\""),
         "channels[P1].conversions[conversion-1].gauge"},
        {replace_once(std::string(kValidCatalog), "gauge_step=\"500\"", "gauge_step=\"0\""),
         "channels[P1].conversions[conversion-1].gauge"},
    };
    for (const auto& [xml, path] : cases)
    {
        expect_import_error(xml, path);
    }
}

TEST(LegacyCdbgCatalogImporter, RejectsUnknownOrRepeatedContentInsideTheSelectedCdbgFields)
{
    const std::vector<std::pair<std::string, std::string_view>> cases = {
        {replace_once(std::string(kValidCatalog), "<protocol id=\"CDBG\">", "<protocol id=\"CDBG\"><unknown/>"),
         "legacy-cdbg.protocol"},
        {replace_once(std::string(kValidCatalog), "id=\"CDBG\"", "id=\"CDBG\" extra=\"x\""), "legacy-cdbg.protocol"},
        {replace_once(std::string(kValidCatalog), " enabled=\"1\"", " extra=\"x\""), "channels[P1]"},
        {replace_once(std::string(kValidCatalog), "<address>0x1234</address>", "<address>0x1234</address><mystery/>"),
         "channels[P1]"},
        {replace_once(std::string(kValidCatalog), "<address>0x1234</address>",
                      "<address>0x1234</address><address>0x20</address>"),
         "channels[P1].address"},
        {replace_once(std::string(kValidCatalog), "</conversions>", "</conversions><conversions/>"),
         "channels[P1].conversions"},
        {replace_once(std::string(kValidCatalog), "gauge_step=\"500\"", "gauge_step=\"500\" extra=\"x\""),
         "channels[P1].conversions[conversion-1]"},
        {replace_once(std::string(kValidCatalog), "gauge_step=\"500\"/>", "gauge_step=\"500\"><unknown/></conversion>"),
         "channels[P1].conversions[conversion-1]"},
        {replace_once(std::string(kValidCatalog), "</conversions>", "<unknown/></conversions>"),
         "channels[P1].conversions"},
    };
    for (const auto& [xml, path] : cases)
    {
        expect_import_error(xml, path);
    }
}

TEST(LegacyCdbgCatalogImporter, RejectsCallerDefaultsThroughCompleteDocumentValidation)
{
    std::vector<std::pair<LegacyCdbgImportDefaults, std::string_view>> cases;
    auto defaults = valid_defaults();
    defaults.document_name.clear();
    cases.emplace_back(defaults, "metadata.name");
    defaults = valid_defaults();
    defaults.bitrate = 0;
    cases.emplace_back(defaults, "connection.bitrate");
    defaults = valid_defaults();
    defaults.identifier_width = static_cast<CanIdentifierWidth>(12);
    cases.emplace_back(defaults, "connection.identifier-width");
    defaults = valid_defaults();
    defaults.sampling_interval_ms = 0;
    cases.emplace_back(defaults, "connection.sampling-interval-ms");
    defaults = valid_defaults();
    defaults.retry.poll_timeout_ms = 0;
    cases.emplace_back(defaults, "connection.retry.poll-timeout-ms");
    defaults = valid_defaults();
    defaults.retry.silence_threshold = 0;
    cases.emplace_back(defaults, "connection.retry.silence-threshold");
    defaults = valid_defaults();
    defaults.retry.reconnect_attempts = 0;
    cases.emplace_back(defaults, "connection.retry.reconnect-attempts");
    defaults = valid_defaults();
    defaults.retry.reconnect_period_ms = 0;
    cases.emplace_back(defaults, "connection.retry.reconnect-period-ms");

    for (const auto& [invalid_defaults, path] : cases)
    {
        expect_import_error(kValidCatalog, path, invalid_defaults);
    }
}
