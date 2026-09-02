#include "src/ui/desktop/definition/definition_header_form.h"

#include <memory>

#include <QApplication>
#include <QGridLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using fastecu::ui::build_header_form;
using fastecu::ui::definition_header_input;
using fastecu::ui::HeaderFormEditors;
using fastecu::ui::line_edit_value;
using fastecu::ui::normalize_xml_suffix;

namespace
{

// QGridLayout and the editors are QWidgets, which abort at construction
// without a live QApplication. fastecu_gtest links plain gtest_main, so
// bring one up via a ::testing::Environment, mirroring QtPortEnvironment in
// src/platform/desktop/common/ports/qt_port_adapters_test.cpp.
class DefinitionFormEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static char program[] = "definition_header_form_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QApplication>(argc, argv);
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *definition_form_environment = ::testing::AddGlobalTestEnvironment(new DefinitionFormEnvironment);

// The field names the real EcuCalDefStructure::DefHeaderNames carries, in
// the order definitionHeaderInput maps them.
const QStringList kNames = {"xmlid",
                            "internalidaddress",
                            "internalidstring",
                            "ecuid",
                            "make",
                            "market",
                            "model",
                            "submodel",
                            "transmission",
                            "year",
                            "flashmethod",
                            "memmodel",
                            "checksummodule",
                            "include",
                            "notes"};

QStringList labels_for(const QStringList& names)
{
    QStringList labels;
    for (const QString& name : names)
    {
        labels.append(name + " label");
    }
    return labels;
}

} // namespace

TEST(BuildHeaderFormTest, MakesALineEditPerFieldAndOneTextEditForNotes)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);

    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    EXPECT_EQ(editors.line_edits.size(), kNames.size() - 1);
    ASSERT_EQ(editors.text_edits.size(), 1);
    EXPECT_EQ(editors.text_edits.at(0)->objectName(), "notes");
}

TEST(BuildHeaderFormTest, SetsObjectNameOnEveryEditor)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);

    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    QStringList seen;
    for (const QLineEdit *editor : editors.line_edits)
    {
        seen.append(editor->objectName());
    }
    EXPECT_THAT(seen, testing::Contains("xmlid"));
    EXPECT_THAT(seen, testing::Contains("checksummodule"));
    EXPECT_THAT(seen, testing::Not(testing::Contains("")));
}

TEST(BuildHeaderFormTest, LeavesEditorsEmptyWhenNoValuesAreSupplied)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);

    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    for (const QLineEdit *editor : editors.line_edits)
    {
        EXPECT_TRUE(editor->text().isEmpty()) << editor->objectName().toStdString();
    }
    EXPECT_TRUE(editors.text_edits.at(0)->toPlainText().isEmpty());
}

TEST(BuildHeaderFormTest, PrefillsEditorsWhenValuesAreSupplied)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid", "notes"};
    const QStringList values = {"CAL123", "EC00456", "some notes"};

    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, values);

    ASSERT_EQ(editors.line_edits.size(), 2);
    EXPECT_EQ(editors.line_edits.at(0)->text(), "CAL123");
    EXPECT_EQ(editors.line_edits.at(1)->text(), "EC00456");
    ASSERT_EQ(editors.text_edits.size(), 1);
    EXPECT_EQ(editors.text_edits.at(0)->toPlainText(), "some notes");
}

TEST(DefinitionHeaderInputTest, MapsEveryFieldByObjectName)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    for (QLineEdit *editor : editors.line_edits)
    {
        editor->setText(editor->objectName() + "-value");
    }
    editors.line_edits.at(1)->setText("2f8000"); // internalidaddress must parse as hex
    editors.text_edits.at(0)->setPlainText("note body");

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->xml_id, "xmlid-value");
    EXPECT_EQ(input->internal_id, "internalidstring-value");
    EXPECT_EQ(input->ecu_id, "ecuid-value");
    EXPECT_EQ(input->metadata.make, "make-value");
    EXPECT_EQ(input->metadata.market, "market-value");
    EXPECT_EQ(input->metadata.model, "model-value");
    EXPECT_EQ(input->metadata.submodel, "submodel-value");
    EXPECT_EQ(input->metadata.transmission, "transmission-value");
    EXPECT_EQ(input->metadata.year, "year-value");
    EXPECT_EQ(input->metadata.flash_method, "flashmethod-value");
    EXPECT_EQ(input->metadata.memory_model, "memmodel-value");
    EXPECT_EQ(input->metadata.checksum_module, "checksummodule-value");
    EXPECT_EQ(input->include, "include-value");
    EXPECT_EQ(input->notes, "note body");
}

TEST(DefinitionHeaderInputTest, ParsesInternalIdAddressAsHex)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "2f8000"});

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    ASSERT_TRUE(input->internal_id_address.has_value());
    EXPECT_EQ(*input->internal_id_address, 0x2f8000U);
}

TEST(DefinitionHeaderInputTest, EmptyInternalIdAddressYieldsNullopt)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "   "});

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    EXPECT_FALSE(input->internal_id_address.has_value());
}

TEST(DefinitionHeaderInputTest, UnparseableInternalIdAddressIsInvalidConfig)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "not-hex"});

    const auto input = definition_header_input(editors);

    ASSERT_FALSE(input.has_value());
    EXPECT_EQ(input.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(input.error().detail, testing::HasSubstr("internal ID address"));
}

TEST(DefinitionHeaderInputTest, TrimsXmlIdButNotTheOtherFields)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"  CAL123  ", "  EC0  "});

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->xml_id, "CAL123");
    EXPECT_EQ(input->ecu_id, "  EC0  ");
}

TEST(LineEditValueTest, ReturnsTheNamedEditorsTextAndEmptyForAnAbsentName)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"CAL123", "EC0"});

    EXPECT_EQ(line_edit_value(editors, "ecuid"), "EC0");
    EXPECT_EQ(line_edit_value(editors, "nosuchfield"), QString());
}

TEST(NormalizeXmlSuffixTest, StripsOneTrailingDotThenAppendsXml)
{
    EXPECT_EQ(normalize_xml_suffix("foo"), "foo.xml");
    EXPECT_EQ(normalize_xml_suffix("foo."), "foo.xml");
    EXPECT_EQ(normalize_xml_suffix("foo.xml"), "foo.xml");
    EXPECT_EQ(normalize_xml_suffix("foo.bar"), "foo.bar.xml");
    EXPECT_EQ(normalize_xml_suffix("/tmp/a b/def."), "/tmp/a b/def.xml");
}
