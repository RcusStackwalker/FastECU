#include "src/backend/logging/logger_conf.h"

#include <format>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::logging::default_selection;
using fastecu::logging::initial_selection;
using fastecu::logging::LoggerDefinition;
using fastecu::logging::LoggerParameter;
using fastecu::logging::LoggerSelection;
using fastecu::logging::LoggerSwitch;
using fastecu::logging::read_selection;
using fastecu::logging::write_selection;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;

bytes::ByteView view(std::string_view text)
{
    return {reinterpret_cast<const bytes::Byte *>(text.data()), text.size()};
}

std::string text_of(bytes::ByteView data)
{
    return std::string(reinterpret_cast<const char *>(data.data()), data.size());
}

LoggerDefinition make_definition(int parameters, int switches, bool all_enabled)
{
    LoggerDefinition definition;
    for (int i = 0; i < parameters; i++)
    {
        LoggerParameter p;
        p.protocol = "SSM";
        p.id = std::format("P{}", i);
        p.enabled = all_enabled || (i % 2 == 0);
        definition.parameters.push_back(std::move(p));
    }
    for (int i = 0; i < switches; i++)
    {
        LoggerSwitch s;
        s.protocol = "SSM";
        s.id = std::format("S{}", i);
        definition.switches.push_back(std::move(s));
    }
    return definition;
}

constexpr std::string_view kConfWithEcu = R"(<config>
    <logger>
        <ecu id="ECUID1">
            <protocol id="SSM">
                <parameters>
                    <gauges>
                        <parameter id="P1" name=""/>
                        <parameter id="P2" name=""/>
                    </gauges>
                    <lower_panel>
                        <parameter id="P3" name=""/>
                    </lower_panel>
                </parameters>
                <switches>
                    <switch id="S1" name=""/>
                </switches>
            </protocol>
        </ecu>
    </logger>
</config>
)";

TEST(ReadSelection, ReturnsTheSelectionWhenTheEcuIsPresent)
{
    const auto result = read_selection(view(kConfWithEcu), "ECUID1", "conf.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->protocol, "SSM");
    EXPECT_THAT((*result)->gauge_ids, ElementsAre("P1", "P2"));
    EXPECT_THAT((*result)->lower_panel_ids, ElementsAre("P3"));
    EXPECT_THAT((*result)->switch_ids, ElementsAre("S1"));
}

TEST(ReadSelection, ReturnsNulloptWhenTheEcuIsAbsent)
{
    const auto result = read_selection(view(kConfWithEcu), "OTHER", "conf.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->has_value());
}

TEST(ReadSelection, RejectsMalformedXml)
{
    const auto result = read_selection(view("<config><logger>"), "ECUID1", "broken.xml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("broken.xml"));
}

TEST(InitialSelection, TakesTheFirstEntriesIgnoringEnabled)
{
    // Every parameter disabled: initial_selection still fills to the caps.
    LoggerDefinition definition = make_definition(30, 30, false);
    for (auto& p : definition.parameters)
    {
        p.enabled = false;
    }

    const LoggerSelection selection = initial_selection(definition);
    EXPECT_EQ(selection.protocol, "SSM");
    EXPECT_THAT(selection.gauge_ids, SizeIs(15));
    EXPECT_THAT(selection.lower_panel_ids, SizeIs(12));
    EXPECT_THAT(selection.switch_ids, SizeIs(20));
    EXPECT_EQ(selection.gauge_ids.front(), "P0");
    EXPECT_EQ(selection.gauge_ids.back(), "P14");
    EXPECT_EQ(selection.lower_panel_ids.back(), "P11");
    EXPECT_EQ(selection.switch_ids.back(), "S19");
}

TEST(DefaultSelection, WalksOnlyEnabledEntriesAndRespectsTheCaps)
{
    // make_definition enables even indices only when all_enabled is false.
    const LoggerDefinition definition = make_definition(60, 5, false);

    const LoggerSelection selection = default_selection(definition);
    EXPECT_THAT(selection.gauge_ids, SizeIs(15));
    EXPECT_THAT(selection.lower_panel_ids, SizeIs(12));
    EXPECT_EQ(selection.gauge_ids.at(0), "P0");
    EXPECT_EQ(selection.gauge_ids.at(1), "P2");
    EXPECT_EQ(selection.gauge_ids.back(), "P28");
}

TEST(DefaultSelection, YieldsAnEmptyProtocolForAnEmptyDefinition)
{
    const LoggerSelection selection = default_selection(LoggerDefinition{});
    EXPECT_THAT(selection.protocol, IsEmpty());
    EXPECT_THAT(selection.gauge_ids, IsEmpty());
}

TEST(WriteSelection, UpdatesAnExistingEcuElement)
{
    LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"X1", "X2"};
    selection.lower_panel_ids = {"X3"};
    selection.switch_ids = {"X4"};

    const auto written = write_selection(view(kConfWithEcu), "ECUID1", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    const auto reread = read_selection(*written, "ECUID1", "conf.xml");
    ASSERT_TRUE(reread.has_value() && reread->has_value());
    EXPECT_THAT((*reread)->gauge_ids, ElementsAre("X1", "X2"));
    EXPECT_THAT((*reread)->lower_panel_ids, ElementsAre("X3"));
    EXPECT_THAT((*reread)->switch_ids, ElementsAre("X4"));
    // Exactly one <ecu> -- an update must not append a duplicate.
    EXPECT_EQ(text_of(*written).find("ECUID1"), text_of(*written).rfind("ECUID1"));
}

TEST(WriteSelection, AppendsANewEcuElementAndKeepsTheExistingOne)
{
    LoggerSelection selection;
    selection.protocol = "CDBG";
    selection.gauge_ids = {"Y1"};

    const auto written = write_selection(view(kConfWithEcu), "ECUID2", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    const auto original = read_selection(*written, "ECUID1", "conf.xml");
    ASSERT_TRUE(original.has_value() && original->has_value());
    EXPECT_THAT((*original)->gauge_ids, ElementsAre("P1", "P2"));

    const auto added = read_selection(*written, "ECUID2", "conf.xml");
    ASSERT_TRUE(added.has_value() && added->has_value());
    EXPECT_EQ((*added)->protocol, "CDBG");
    EXPECT_THAT((*added)->gauge_ids, ElementsAre("Y1"));
}

// TRANSITIONAL (5d-5b): pins pugixml's output against the bytes
// QDomDocument::save(output, 4) produced, captured by Task 1's
// logger_conf_writes_four_space_indented_xml
// (.superpowers/sdd/2026-08-06-step5d5b-logger-definition-glue/task-1-report.md).
// That test drove read_logger_conf's ECU-not-found branch with a starting
// document of `<config><logger></logger></config>`, a logger definition
// carrying log_value_protocol = {"SSM"} and a single enabled parameter
// "P1" and switch "S1" (log_value_id/log_value_enabled and
// log_switch_id/log_switch_enabled each of length 1), and ecu_id
// "ECUID1". That seeding walk fills dashboard_log_value_id AND
// lower_panel_log_value_id from the same enabled-parameters loop (see
// file_actions.cpp:972-986), so both selections end up {"P1"} -- this
// fixture reproduces that input exactly via write_selection's signature
// by setting protocol/gauge_ids/lower_panel_ids/switch_ids directly,
// which is the input write_selection actually consumes; the
// default_selection() walk that would normally produce that
// LoggerSelection is exercised separately above.
//
// One byte differs from Task 1's captured QDom golden and cannot be closed
// by any pugixml flag combination: pugixml's serializer unconditionally
// writes a space before a self-closing tag's `/>` (pugixml.cpp,
// node_output_start, `if ((flags & format_raw) == 0) writer.write(' ');`)
// unless format_raw is set -- and format_raw also suppresses all
// indentation (pugixml.cpp: indent_length is forced to 0 and newlines are
// skipped whenever format_raw is set), so it cannot be combined with the
// four-space indent this test exists to pin. `<parameter id="P1" name="" />`
// (space before `/>`) is therefore what write_selection actually and
// permanently produces, versus QDom's `<parameter id="P1" name=""/>`. This
// is the one place the golden below diverges from Task 1's captured bytes;
// everything else -- no XML declaration, four-space indent per level, no
// tabs, single trailing newline -- matches exactly.
//
// This is the only place the old writer is treated as authoritative. The
// follow-up issue replaces it with a pugixml-generated golden plus the
// round-trip assertion above; the four-space indent choice itself stays.
TEST(WriteSelection, ReproducesTheFourSpaceQDomIndent)
{
    LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"P1"};
    selection.lower_panel_ids = {"P1"};
    selection.switch_ids = {"S1"};

    const auto written = write_selection(
        view("<config><logger></logger></config>"), "ECUID1", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    // Task 1's captured QDom golden, with the one byte-per-self-closing-tag
    // adjustment documented above (` />` instead of `/>`): no XML
    // declaration, four-space indent per level, no tabs, single trailing
    // newline.
    constexpr std::string_view kPugixmlGolden =
        "<config>\n"
        "    <logger>\n"
        "        <ecu id=\"ECUID1\">\n"
        "            <protocol id=\"SSM\">\n"
        "                <parameters>\n"
        "                    <gauges>\n"
        "                        <parameter id=\"P1\" name=\"\" />\n"
        "                    </gauges>\n"
        "                    <lower_panel>\n"
        "                        <parameter id=\"P1\" name=\"\" />\n"
        "                    </lower_panel>\n"
        "                </parameters>\n"
        "                <switches>\n"
        "                    <switch id=\"S1\" name=\"\" />\n"
        "                </switches>\n"
        "            </protocol>\n"
        "        </ecu>\n"
        "    </logger>\n"
        "</config>\n";

    const std::string xml = text_of(*written);
    EXPECT_EQ(xml, kPugixmlGolden);
}

} // namespace
