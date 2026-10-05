#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/definition/definition_header_form.h"

#include <array>
#include <memory>

#include <QApplication>
#include <QDomDocument>
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
        static auto program = std::to_array("definition_header_form_test");
        static auto argv = std::to_array<char *>({program.data(), nullptr});
        app_ = std::make_unique<QApplication>(argc, argv.data());
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *definition_form_environment = ::testing::AddGlobalTestEnvironment(new DefinitionFormEnvironment);

// The authored header field names, in the order the form maps them.
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

    ASSERT_THAT(input, fastecu::testing::IsOk());
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

    ASSERT_THAT(input, fastecu::testing::IsOk());
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

    ASSERT_THAT(input, fastecu::testing::IsOk());
    EXPECT_FALSE(input->internal_id_address.has_value());
}

TEST(DefinitionHeaderInputTest, UnparseableInternalIdAddressIsInvalidConfig)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "not-hex"});

    const auto input = definition_header_input(editors);

    ASSERT_THAT(input, fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
    EXPECT_THAT(input.error().detail, testing::HasSubstr("internal ID address"));
}

TEST(DefinitionHeaderInputTest, TrimsXmlIdButNotTheOtherFields)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"  CAL123  ", "  EC0  "});

    const auto input = definition_header_input(editors);

    ASSERT_THAT(input, fastecu::testing::IsOk());
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

TEST(ImportedHeaderFieldsTest, PreservesFieldOrderAndDefaultsAbsentOptionalFields)
{
    const QString xml = "<rom><romid><xmlid>BASE_TEST</xmlid>"
                        "<internalidaddress>0x2000</internalidaddress><internalidstring>TESTID</internalidstring>"
                        "<ecuid>TESTECU</ecuid><make>Subaru</make><market>USDM</market><model>Impreza</model>"
                        "<year>2004</year><flashmethod>sub_ecu_denso_sh7055</flashmethod><memmodel>SH7055</memmodel>"
                        "<checksummodule>checksum_ecu_subaru_denso_sh7055</checksummodule></romid></rom>";

    const auto fields = fastecu::ui::collect_ecuflash_base_header_fields(kNames, {xml});

    ASSERT_EQ(fields.size(), kNames.size() * 2);
    const QStringList expectedValues = {"BASE_TEST",
                                        "0x2000",
                                        "TESTID",
                                        "TESTECU",
                                        "Subaru",
                                        "USDM",
                                        "Impreza",
                                        "",
                                        "",
                                        "2004",
                                        "sub_ecu_denso_sh7055",
                                        "SH7055",
                                        "checksum_ecu_subaru_denso_sh7055",
                                        "",
                                        ""};
    for (qsizetype index = 0; index < kNames.size(); ++index)
    {
        EXPECT_EQ(fields.at(index * 2), kNames.at(index));
        EXPECT_EQ(fields.at(index * 2 + 1), expectedValues.at(index));
    }
}

TEST(ImportedHeaderFieldsTest, ReadsIncludeAndNotesFromWrappedRom)
{
    const QStringList names{"xmlid", "include", "notes"};
    const QStringList lines{"<roms><rom><romid><xmlid>  BASE  </xmlid></romid>",
                            "<include>OEM_BASE</include><notes>Text &amp; notes</notes></rom></roms>"};

    EXPECT_EQ(fastecu::ui::collect_ecuflash_base_header_fields(names, lines),
              (QStringList{"xmlid", "  BASE  ", "include", "OEM_BASE", "notes", "Text & notes"}));
}

TEST(ImportedHeaderFieldsTest, MalformedXmlLeavesEveryRequestedFieldBlank)
{
    const QStringList names{"xmlid", "include", "notes"};

    EXPECT_EQ(fastecu::ui::collect_ecuflash_base_header_fields(names, {"<rom><romid>"}),
              (QStringList{"xmlid", "", "include", "", "notes", ""}));
}

TEST(DefinitionHeaderInputTest, RetainsQtAddressAndUnicodeWhitespaceSemantics)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {});
    for (const QString& text : QStringList{"+0x10", "-0", "+ 10", "ffffffffffffffff", "10000000000000000",
                                           QString::fromUtf8("\xc2\xa0") + "10" + QString::fromUtf8("\xe3\x80\x80")})
    {
        SCOPED_TRACE(text.toStdString());
        editors.line_edits.at(0)->setText(QString::fromUtf8("\xe2\x80\x83") + "ID" + QString::fromUtf8("\xc2\xa0"));
        editors.line_edits.at(1)->setText(text);
        bool valid = false;
        const auto expected = text.trimmed().toULongLong(&valid, 16);
        const auto input = definition_header_input(editors);
        if (valid)
        {
            ASSERT_THAT(input, fastecu::testing::IsOk());
            EXPECT_EQ(input->xml_id, "ID");
            EXPECT_EQ(input->internal_id_address, expected);
        }
        else
        {
            EXPECT_THAT(input, fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
        }
    }
}

TEST(ImportedHeaderFieldsTest, PortableExtractionAgreesWithTheFormerQtParser)
{
    const QStringList names{"xmlid", "include", "notes", "model", "xmlid"};
    for (const QString& source : QStringList{
             "<rom><romid><xmlid> ID </xmlid></romid><notes>Text &amp; <b>nested</b><![CDATA[ notes]]></notes></rom>",
             "<roms><!-- comment --><rom><romid><xmlid>ID</xmlid></romid><include>BASE</include></rom></roms>",
             "<rom><romid><xmlid>first</xmlid><xmlid>second</xmlid></romid><notes>   </notes></rom>", "<rom><romid>",
             "<rom><romid><xmlid>A&bogus;B</xmlid></romid></rom>", "<rom><romid><xmlid>A&#0;B</xmlid></romid></rom>",
             "<!DOCTYPE rom [<!ENTITY id 'BASE'>]><rom><romid><xmlid>&id;</xmlid></romid></rom>",
             QString::fromUtf8("<!DOCTYPE rom [<!ENTITY id 'BASE'><!ENTITY name "
                               "'&id;_NAME'>]><rom><romid><xmlid>&name;</xmlid></romid></rom>"),
             "<!DOCTYPE rom [<!ENTITY note '<b>nested</b>'>]><rom><romid/><notes>&note;</notes></rom>",
             QString::fromUtf8(
                 "<!DOCTYPE rom [<!ENTITY % declarations \"<!ENTITY id 'FIRST'>\">%declarations;<!ENTITY id "
                 "'SECOND'>]><rom><romid><xmlid>&id;</xmlid></romid></rom>"),
             QString::fromUtf8("<!DOCTYPE rom [<!ENTITY markup '&#60;b>markup&#60;/b>'><!ENTITY literal "
                               "'&lt;b>text&lt;/b>'>]><rom><notes>&markup; &literal;</notes></rom>"),
             QString::fromUtf8("<!DOCTYPE rom [<!ENTITY external SYSTEM "
                               "'file:///unavailable'>]><rom><romid><xmlid>A&external;B</xmlid></romid></rom>"),
             "<!DOCTYPE rom SYSTEM 'file:///unavailable'><rom><romid><xmlid>A&missing;B</xmlid></romid></rom>",
             QString::fromUtf8("<!DOCTYPE rom [<!ENTITY external SYSTEM 'file:///unavailable'>]><rom "
                               "unused='&external;'><romid><xmlid>ID</xmlid></romid></rom>"),
             "<!DOCTYPE rom [<!ENTITY id '&id;'>]><rom><romid><xmlid>&id;</xmlid></romid></rom>",
             "<rom><!-- &undefined; --><notes><![CDATA[A&undefined;B]]></notes></rom>",
             "<rom><romid><xmlid>first</xmlid></romid></rom><rom/>",
             QString::fromUtf8("<?xml version=\"1.0\" "
                               "encoding=\"ISO-8859-1\"?><rom><romid><xmlid>Caf\xc3\xa9</xmlid></romid></rom>")})
    {
        SCOPED_TRACE(source.toStdString());
        QDomDocument document;
        QDomElement root;
        if (document.setContent(source))
        {
            root = document.documentElement();
            for (int depth = 0; !root.isNull() && root.tagName() != "rom" && depth < 5; ++depth)
            {
                root = root.firstChildElement();
            }
        }
        const auto rom_id = root.firstChildElement("romid");
        QStringList expected;
        for (const auto& name : names)
        {
            const auto element =
                (name == "include" || name == "notes") ? root.firstChildElement(name) : rom_id.firstChildElement(name);
            expected << name << element.text();
        }
        EXPECT_EQ(fastecu::ui::collect_ecuflash_base_header_fields(names, {source}), expected);
    }
}

TEST(DefinitionHeaderInputTest, TrimmingAgreesWithQtForUnicodeWhitespaceAndNonWhitespace)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names{"xmlid", "internalidaddress"};
    const auto editors = build_header_form(grid, labels_for(names), names, {});
    for (const int code : {0x09,   0x0a,   0x0b,   0x0c,   0x0d,   0x20,   0x85,   0xa0,   0x1680, 0x2000,
                           0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a,
                           0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0x180e, 0x200b, 0xfeff})
    {
        SCOPED_TRACE(code);
        const QString padding{QChar{static_cast<char16_t>(code)}};
        const QString xml_id = padding + "ID" + padding;
        const QString address = padding + "10" + padding;
        editors.line_edits.at(0)->setText(xml_id);
        editors.line_edits.at(1)->setText(address);
        bool valid = false;
        const auto expected = address.trimmed().toULongLong(&valid, 16);
        const auto input = definition_header_input(editors);
        if (valid)
        {
            ASSERT_THAT(input, fastecu::testing::IsOk());
            EXPECT_EQ(input->xml_id, xml_id.trimmed().toStdString());
            EXPECT_EQ(input->internal_id_address, expected);
        }
        else
        {
            EXPECT_THAT(input, fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
        }
    }
}
