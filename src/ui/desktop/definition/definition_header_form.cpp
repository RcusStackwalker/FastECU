#include "src/ui/desktop/definition/definition_header_form.h"

#include <cstdint>
#include <optional>

#include <QHash>
#include <QLabel>

namespace fastecu::ui
{

HeaderFormEditors build_header_form(QGridLayout *grid, const QStringList& labels, const QStringList& names,
                                    const QStringList& values)
{
    HeaderFormEditors editors;
    for (int index = 0; index < names.length(); index++)
    {
        auto *label = new QLabel(labels.at(index));
        grid->addWidget(label, index, 0);

        const QString value = index < values.length() ? values.at(index) : QString();
        if (names.at(index) == "notes")
        {
            auto *editor = new QTextEdit();
            editor->setObjectName(names.at(index));
            editor->setText(value);
            // One row lower and spanning both columns, as legacy did.
            grid->addWidget(editor, index + 1, 0, 1, 2);
            editors.text_edits.append(editor);
        }
        else
        {
            auto *editor = new QLineEdit();
            editor->setObjectName(names.at(index));
            editor->setText(value);
            grid->addWidget(editor, index, 1);
            editors.line_edits.append(editor);
        }
    }
    return editors;
}

fastecu::Result<fastecu::definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors)
{
    QHash<QString, QString> fields;
    for (const QLineEdit *editor : editors.line_edits)
    {
        fields.insert(editor->objectName(), editor->text());
    }
    for (const QTextEdit *editor : editors.text_edits)
    {
        fields.insert(editor->objectName(), editor->toPlainText());
    }

    std::optional<std::uint64_t> internalIdAddress;
    const QString addressText = fields.value("internalidaddress").trimmed();
    if (!addressText.isEmpty())
    {
        bool validAddress = false;
        const std::uint64_t parsedAddress = addressText.toULongLong(&validAddress, 16);
        if (!validAddress)
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "definition internal ID address is not a valid integer");
        }
        internalIdAddress = parsedAddress;
    }

    return fastecu::definition::DefinitionHeaderInput{
        .xml_id = fields.value("xmlid").trimmed().toStdString(),
        .internal_id = fields.value("internalidstring").toStdString(),
        .ecu_id = fields.value("ecuid").toStdString(),
        .internal_id_address = internalIdAddress,
        .metadata =
            fastecu::definition::RomMetadata{
                .make = fields.value("make").toStdString(),
                .market = fields.value("market").toStdString(),
                .model = fields.value("model").toStdString(),
                .submodel = fields.value("submodel").toStdString(),
                .transmission = fields.value("transmission").toStdString(),
                .year = fields.value("year").toStdString(),
                .flash_method = fields.value("flashmethod").toStdString(),
                .memory_model = fields.value("memmodel").toStdString(),
                .checksum_module = fields.value("checksummodule").toStdString(),
            },
        .include = fields.value("include").toStdString(),
        .notes = fields.value("notes").toStdString(),
    };
}

QString line_edit_value(const HeaderFormEditors& editors, const QString& name)
{
    for (const QLineEdit *editor : editors.line_edits)
    {
        if (editor->objectName() == name)
        {
            return editor->text();
        }
    }
    return {};
}

QString normalize_xml_suffix(QString filename)
{
    if (filename.endsWith(QString(".")))
    {
        filename.remove(filename.length() - 1, 1);
    }
    if (!filename.endsWith(QString(".xml")))
    {
        filename.append(QString(".xml"));
    }
    return filename;
}

} // namespace fastecu::ui
