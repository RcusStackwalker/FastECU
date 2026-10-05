#include "src/ui/desktop/definition/definition_header_form.h"

#include <QLabel>

namespace fastecu::ui
{

HeaderFormEditors build_header_form(QGridLayout *grid, const definition::DefinitionHeaderDraft& draft)
{
    int row = 0;
    const auto add_line = [&](const char *label, const char *name, const std::string& value)
    {
        grid->addWidget(new QLabel(QString::fromUtf8(label)), row, 0);
        auto *editor = new QLineEdit(QString::fromStdString(value));
        editor->setObjectName(QString::fromUtf8(name));
        grid->addWidget(editor, row++, 1);
        return editor;
    };
    HeaderFormEditors editors{
        .xml_id = add_line("XML ID", "xmlid", draft.xml_id),
        .internal_id_address = add_line("Internal ID Address", "internalidaddress", draft.internal_id_address_text),
        .internal_id = add_line("Internal ID String", "internalidstring", draft.internal_id),
        .ecu_id = add_line("ECU ID", "ecuid", draft.ecu_id),
        .make = add_line("Make", "make", draft.metadata.make),
        .market = add_line("Market", "market", draft.metadata.market),
        .model = add_line("Model", "model", draft.metadata.model),
        .submodel = add_line("Submodel", "submodel", draft.metadata.submodel),
        .transmission = add_line("Transmission", "transmission", draft.metadata.transmission),
        .year = add_line("Year", "year", draft.metadata.year),
        .flash_method = add_line("Flash Method", "flashmethod", draft.metadata.flash_method),
        .memory_model = add_line("Memory Model", "memmodel", draft.metadata.memory_model),
        .checksum_module = add_line("Checksum Module", "checksummodule", draft.metadata.checksum_module),
        .include = add_line("Include", "include", draft.include),
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
    return definition::definition_header_input(read_header_form(editors));
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
