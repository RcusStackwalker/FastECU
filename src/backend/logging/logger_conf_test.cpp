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
using ::testing::Not;
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
        s.enabled = all_enabled || (i % 2 == 0);
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
    // Every parameter and switch disabled: initial_selection still fills to
    // the caps -- the first-N walk does not gate on `enabled` for either.
    LoggerDefinition definition = make_definition(30, 30, false);
    for (auto& p : definition.parameters)
    {
        p.enabled = false;
    }
    for (auto& s : definition.switches)
    {
        s.enabled = false;
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
    // make_definition enables even indices only when all_enabled is false --
    // for both parameters and switches.
    const LoggerDefinition definition = make_definition(60, 5, false);

    const LoggerSelection selection = default_selection(definition);
    EXPECT_THAT(selection.gauge_ids, SizeIs(15));
    EXPECT_THAT(selection.lower_panel_ids, SizeIs(12));
    EXPECT_EQ(selection.gauge_ids.at(0), "P0");
    EXPECT_EQ(selection.gauge_ids.at(1), "P2");
    EXPECT_EQ(selection.gauge_ids.back(), "P28");
    // Switches S0, S2, S4 are enabled; S1, S3 are not.
    EXPECT_THAT(selection.switch_ids, ElementsAre("S0", "S2", "S4"));
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

    // Task 1's captured QDom golden, verbatim (537 bytes): no XML
    // declaration, four-space indent per level, no tabs, no space before a
    // self-closing tag's `/>`, single trailing newline. write_selection's
    // post-serialization " />\n" -> "/>\n" fixup (logger_conf.cpp) is what
    // makes this a literal match despite pugixml's serializer always writing
    // that space itself.
    constexpr std::string_view kQDomGolden =
        "<config>\n"
        "    <logger>\n"
        "        <ecu id=\"ECUID1\">\n"
        "            <protocol id=\"SSM\">\n"
        "                <parameters>\n"
        "                    <gauges>\n"
        "                        <parameter id=\"P1\" name=\"\"/>\n"
        "                    </gauges>\n"
        "                    <lower_panel>\n"
        "                        <parameter id=\"P1\" name=\"\"/>\n"
        "                    </lower_panel>\n"
        "                </parameters>\n"
        "                <switches>\n"
        "                    <switch id=\"S1\" name=\"\"/>\n"
        "                </switches>\n"
        "            </protocol>\n"
        "        </ecu>\n"
        "    </logger>\n"
        "</config>\n";

    const std::string xml = text_of(*written);
    EXPECT_EQ(xml, kQDomGolden);
}

// Fix round 1, finding 1: document.save() re-serializes the whole DOM, so an
// untouched sibling <ecu>'s self-closing elements get pugixml's space before
// `/>` too, not just the rebuilt subtree -- proving the fixup above is not
// scoped to the <ecu> this call touches.
TEST(WriteSelection, StripsTheSelfClosingSpaceFromUntouchedSiblingsToo)
{
    constexpr std::string_view kTwoEcus = R"(<config>
    <logger>
        <ecu id="ECUID1">
            <protocol id="SSM">
                <parameters>
                    <gauges>
                        <parameter id="Z9" name="untouched"/>
                    </gauges>
                    <lower_panel/>
                </parameters>
                <switches/>
            </protocol>
        </ecu>
    </logger>
</config>
)";

    LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"Y1"};

    const auto written = write_selection(view(kTwoEcus), "ECUID2", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    const std::string xml = text_of(*written);
    EXPECT_THAT(xml, HasSubstr("<parameter id=\"Z9\" name=\"untouched\"/>"));
    EXPECT_THAT(xml, Not(HasSubstr("untouched\" />")));
}

// Fix round 1, finding 2: parse_default excludes comments; without
// parse_comments a hand-annotated conf file loses the annotation silently on
// the very first write_selection() round-trip.
TEST(WriteSelection, PreservesAnExistingCommentThroughARoundTrip)
{
    constexpr std::string_view kConfWithComment =
        "<config>\n"
        "    <!-- user note: don't touch ECUID1 -->\n"
        "    <logger/>\n"
        "</config>\n";

    LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"P1"};

    const auto written =
        write_selection(view(kConfWithComment), "ECUID1", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    EXPECT_THAT(text_of(*written), HasSubstr("<!-- user note: don't touch ECUID1 -->"));
}

} // namespace
