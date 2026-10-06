#pragma once

#include <QGridLayout>
#include <QLineEdit>
#include <QString>
#include <QTextEdit>

#include "src/backend/definition/definition_header_fields.h"

namespace fastecu::ui
{

// Borrowed controls for the fixed header form; the containing dialog owns them.
struct HeaderFormEditors
{
    QLineEdit *xml_id{};
    QLineEdit *internal_id_address{};
    QLineEdit *internal_id{};
    QLineEdit *ecu_id{};
    QLineEdit *make{};
    QLineEdit *market{};
    QLineEdit *model{};
    QLineEdit *submodel{};
    QLineEdit *transmission{};
    QLineEdit *year{};
    QLineEdit *flash_method{};
    QLineEdit *memory_model{};
    QLineEdit *checksum_module{};
    QLineEdit *include{};
    QTextEdit *notes{};
};

// Build the fixed form from named domain values; presentation stays in the UI.
HeaderFormEditors build_header_form(QGridLayout *grid, const definition::DefinitionHeaderDraft& draft = {});
definition::DefinitionHeaderDraft read_header_form(const HeaderFormEditors& editors);
Result<definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors);

// Strip a trailing dot and append .xml when absent.
QString normalize_xml_suffix(QString filename);

} // namespace fastecu::ui
