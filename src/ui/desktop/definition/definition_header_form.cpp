#include "src/ui/desktop/definition/definition_header_form.h"

#include <vector>

#include <QLabel>

#include "src/backend/definition/definition_header_fields.h"

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

QStringList collect_ecuflash_base_header_fields(const QStringList& header_names, const QStringList& definition_lines)
{
    std::vector<std::string> names;
    names.reserve(header_names.size());
    for (const auto& name : header_names)
    {
        names.push_back(name.toStdString());
    }
    const auto fields =
        fastecu::definition::collect_ecuflash_base_header_fields(names, definition_lines.join(QString()).toStdString());
    QStringList values;
    for (const auto& [name, text] : fields)
    {
        values << QString::fromStdString(name) << QString::fromStdString(text);
    }
    return values;
}

fastecu::Result<fastecu::definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors)
{
    fastecu::definition::DefinitionHeaderFields fields;
    fields.reserve(editors.line_edits.size() + editors.text_edits.size());
    for (const QLineEdit *editor : editors.line_edits)
    {
        fields.emplace_back(editor->objectName().toStdString(), editor->text().toStdString());
    }
    for (const QTextEdit *editor : editors.text_edits)
    {
        fields.emplace_back(editor->objectName().toStdString(), editor->toPlainText().toStdString());
    }
    return fastecu::definition::definition_header_input(fields);
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
