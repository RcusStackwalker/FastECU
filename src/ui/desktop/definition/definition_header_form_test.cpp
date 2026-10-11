#include "src/ui/desktop/definition/definition_header_form.h"

#include <array>
#include <memory>

#include <QApplication>
#include <QLabel>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

using fastecu::definition::DefinitionHeaderDraft;
using fastecu::ui::buildHeaderForm;
using fastecu::ui::definitionHeaderInput;
using fastecu::ui::normalizeXmlSuffix;

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
        static auto program = std::to_array("definition_header_form_test");
        static auto argv = std::to_array<char *>({program.data(), nullptr});
        app_ = std::make_unique<QApplication>(argc, argv.data());
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *const kDefinitionFormEnvironment = ::testing::AddGlobalTestEnvironment(new DefinitionFormEnvironment);

} // namespace

TEST(BuildHeaderFormTest, MapsTypedValuesAndEditsBackIntoTheDomainHeader)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const DefinitionHeaderDraft draft{.xml_id = "XML",
                                      .internal_id = "INTERNAL",
                                      .ecu_id = "ECU",
                                      .internal_id_address_text = "2f8000",
                                      .metadata = {.make = "Make",
                                                   .market = "Market",
                                                   .model = "Model",
                                                   .submodel = "Submodel",
                                                   .transmission = "Transmission",
                                                   .year = "Year",
                                                   .flash_method = "Flash",
                                                   .memory_model = "Memory",
                                                   .checksum_module = "Checksum"},
                                      .include = "Parent",
                                      .notes = "Notes"};
    const auto editors = buildHeaderForm(grid, draft);
    EXPECT_EQ(editors.xml_id->text(), "XML");
    EXPECT_EQ(editors.internal_id_address->text(), "2f8000");
    EXPECT_EQ(editors.internal_id->text(), "INTERNAL");
    EXPECT_EQ(editors.ecu_id->text(), "ECU");
    EXPECT_EQ(editors.make->text(), "Make");
    EXPECT_EQ(editors.market->text(), "Market");
    EXPECT_EQ(editors.model->text(), "Model");
    EXPECT_EQ(editors.submodel->text(), "Submodel");
    EXPECT_EQ(editors.transmission->text(), "Transmission");
    EXPECT_EQ(editors.year->text(), "Year");
    EXPECT_EQ(editors.flash_method->text(), "Flash");
    EXPECT_EQ(editors.memory_model->text(), "Memory");
    EXPECT_EQ(editors.checksum_module->text(), "Checksum");
    EXPECT_EQ(editors.include->text(), "Parent");
    EXPECT_EQ(editors.notes->toPlainText(), "Notes");
    editors.ecu_id->setText("Changed ECU");
    const auto input = definitionHeaderInput(editors);
    ASSERT_THAT(input, fastecu::testing::IsOk());
    EXPECT_EQ(input->xml_id, "XML");
    EXPECT_EQ(input->internal_id, "INTERNAL");
    EXPECT_EQ(input->ecu_id, "Changed ECU");
    EXPECT_EQ(input->internal_id_address, fastecu::memory::DefinitionAddress{0x2f8000});
    EXPECT_EQ(input->metadata, draft.metadata);
    EXPECT_EQ(input->include, "Parent");
    EXPECT_EQ(input->notes, "Notes");
}

TEST(BuildHeaderFormTest, PreservesFieldLabelsAndPlacement)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const auto editors = buildHeaderForm(grid);
    const auto labels = std::to_array<const char *>(
        {"XML ID", "Internal ID Address", "Internal ID String", "ECU ID", "Make", "Market", "Model", "Submodel",
         "Transmission", "Year", "Flash Method", "Memory Model", "Checksum Module", "Include", "Notes"});
    for (std::size_t row = 0; row < labels.size(); ++row)
    {
        const auto *label = qobject_cast<QLabel *>(grid->itemAtPosition(static_cast<int>(row), 0)->widget());
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label->text(), QString::fromUtf8(labels[row]));
    }
    EXPECT_EQ(grid->itemAtPosition(0, 1)->widget(), editors.xml_id);
    EXPECT_EQ(grid->itemAtPosition(15, 0)->widget(), editors.notes);
    EXPECT_TRUE(editors.xml_id->text().isEmpty());
    EXPECT_TRUE(editors.notes->toPlainText().isEmpty());
}

TEST(DefinitionHeaderInputTest, WidgetObjectNamesDoNotDetermineDomainFieldMapping)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const auto editors = buildHeaderForm(grid, {.xml_id = "ID", .ecu_id = "ECU"});
    editors.ecu_id->setObjectName("presentation-only");
    const auto input = definitionHeaderInput(editors);
    ASSERT_THAT(input, fastecu::testing::IsOk());
    EXPECT_EQ(input->ecu_id, "ECU");
}

TEST(DefinitionHeaderInputTest, NormalizesScalarsAndValidatesEditedAddress)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const auto editors =
        buildHeaderForm(grid, {.xml_id = "  CAL123  ", .ecu_id = "  ECU  ", .internal_id_address_text = "   "});
    auto input = definitionHeaderInput(editors);
    ASSERT_THAT(input, fastecu::testing::IsOk());
    EXPECT_EQ(input->xml_id, "CAL123");
    EXPECT_EQ(input->ecu_id, "ECU");
    EXPECT_EQ(input->internal_id_address, std::nullopt);
    editors.internal_id_address->setText("not-hex");
    EXPECT_THAT(definitionHeaderInput(editors), fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(NormalizeXmlSuffixTest, StripsOneTrailingDotThenAppendsXml)
{
    EXPECT_EQ(normalizeXmlSuffix("foo"), "foo.xml");
    EXPECT_EQ(normalizeXmlSuffix("foo."), "foo.xml");
    EXPECT_EQ(normalizeXmlSuffix("foo.xml"), "foo.xml");
    EXPECT_EQ(normalizeXmlSuffix("foo.bar"), "foo.bar.xml");
    EXPECT_EQ(normalizeXmlSuffix("/tmp/a b/def."), "/tmp/a b/def.xml");
}

TEST(ImportedHeaderFieldsTest, LiteralMarkupNotesSurviveExtractionFormAndWriting)
{
    const auto draft = fastecu::definition::ReadDefinitionHeader(
        "<rom><romid><xmlid>ID</xmlid><internalidstring>INTERNAL</internalidstring><ecuid>ECU</ecuid>"
        "</romid><notes><![CDATA[ \n<b>literal note</b>\n ]]></notes></rom>");
    ASSERT_THAT(draft, fastecu::testing::IsOk());
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const auto editors = buildHeaderForm(grid, *draft);
    const auto input = definitionHeaderInput(editors);
    ASSERT_THAT(input, fastecu::testing::IsOk());
    EXPECT_EQ(input->notes, " \n<b>literal note</b>\n ");
    const auto written = fastecu::definition::CreateEcuflashXml(*input);
    ASSERT_THAT(written, fastecu::testing::IsOk());
    EXPECT_THAT(std::string(written->begin(), written->end()),
                testing::HasSubstr("<notes> \n&lt;b&gt;literal note&lt;/b&gt;\n </notes>"));
}
