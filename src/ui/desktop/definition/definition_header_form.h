#pragma once
#include <QGridLayout>
#include <QLineEdit>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTextEdit>

#include "src/backend/definition/definition_writer.h"
#include "src/backend/ports/result.h"

namespace fastecu::ui
{

// The editors of one ROM-header form, keyed by each editor's objectName().
// `notes` is the only field rendered as a QTextEdit; everything else is a
// QLineEdit. Both lists are non-owning -- the QGridLayout passed to
// build_header_form owns the widgets through Qt's parent chain.
struct HeaderFormEditors
{
    QList<QLineEdit *> line_edits;
    QList<QTextEdit *> text_edits;
};

// Populates `grid` with one labelled row per entry in `names`, and returns
// the editors created. `labels` supplies the human-readable row labels and
// must be at least as long as `names`. `values` prefills the editors when
// non-empty; pass {} to leave every field blank. The `notes` field becomes a
// QTextEdit placed one row lower and spanning both columns, matching the
// legacy layout.
HeaderFormEditors build_header_form(QGridLayout *grid, const QStringList& labels, const QStringList& names,
                                    const QStringList& values);

// Maps the form's editors onto a DefinitionHeaderInput by objectName().
// `internalidaddress` is parsed as hex: empty yields nullopt, unparseable
// yields ErrorKind::InvalidConfig. Only xmlid and internalidaddress are
// trimmed -- every other field is taken verbatim, matching legacy.
fastecu::Result<fastecu::definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors);

// The raw text of the line edit with this objectName, or an empty QString
// when no such editor exists.
QString line_edit_value(const HeaderFormEditors& editors, const QString& name);

// Strips a single trailing '.' then appends ".xml" when not already
// present. Order matters: "foo." becomes "foo.xml", not "foo..xml".
QString normalize_xml_suffix(QString filename);

} // namespace fastecu::ui
