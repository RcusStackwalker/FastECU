#include "src/ui/desktop/definition/definition_header_form.h"

#include <QLabel>

namespace fastecu::ui
{

HeaderFormEditors build_header_form(QGridLayout *grid, const definition::DefinitionHeaderDraft& draft)
{
    int row = 0;
    const auto addLine = [&](const char *label, const char *name, const std::string& value)
    {
        grid->addWidget(new QLabel(QString::fromUtf8(label)), row, 0);
        auto *editor = new QLineEdit(QString::fromStdString(value));
        editor->setObjectName(QString::fromUtf8(name));
        grid->addWidget(editor, row++, 1);
        return editor;
    };
    HeaderFormEditors editors{
        .xml_id = addLine("XML ID", "xmlid", draft.xml_id),
        .internal_id_address = addLine("Internal ID Address", "internalidaddress", draft.internal_id_address_text),
        .internal_id = addLine("Internal ID String", "internalidstring", draft.internal_id),
        .ecu_id = addLine("ECU ID", "ecuid", draft.ecu_id),
        .make = addLine("Make", "make", draft.metadata.make),
        .market = addLine("Market", "market", draft.metadata.market),
        .model = addLine("Model", "model", draft.metadata.model),
        .submodel = addLine("Submodel", "submodel", draft.metadata.submodel),
        .transmission = addLine("Transmission", "transmission", draft.metadata.transmission),
        .year = addLine("Year", "year", draft.metadata.year),
        .flash_method = addLine("Flash Method", "flashmethod", draft.metadata.flash_method),
        .memory_model = addLine("Memory Model", "memmodel", draft.metadata.memory_model),
        .checksum_module = addLine("Checksum Module", "checksummodule", draft.metadata.checksum_module),
        .include = addLine("Include", "include", draft.include),
        .notes = new QTextEdit(),
    };
    grid->addWidget(new QLabel("Notes"), row, 0);
    editors.notes->setObjectName("notes");
    editors.notes->setPlainText(QString::fromStdString(draft.notes));
    grid->addWidget(editors.notes, row + 1, 0, 1, 2);
    return editors;
}

definition::DefinitionHeaderDraft read_header_form(const HeaderFormEditors& editors)
{
    return {.xml_id = editors.xml_id->text().toStdString(),
            .internal_id = editors.internal_id->text().toStdString(),
            .ecu_id = editors.ecu_id->text().toStdString(),
            .internal_id_address_text = editors.internal_id_address->text().toStdString(),
            .metadata = {.make = editors.make->text().toStdString(),
                         .market = editors.market->text().toStdString(),
                         .model = editors.model->text().toStdString(),
                         .submodel = editors.submodel->text().toStdString(),
                         .transmission = editors.transmission->text().toStdString(),
                         .year = editors.year->text().toStdString(),
                         .flash_method = editors.flash_method->text().toStdString(),
                         .memory_model = editors.memory_model->text().toStdString(),
                         .checksum_module = editors.checksum_module->text().toStdString()},
            .include = editors.include->text().toStdString(),
            .notes = editors.notes->toPlainText().toStdString()};
}

Result<definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors)
{
    return definition::BuildDefinitionHeaderInput(read_header_form(editors));
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
