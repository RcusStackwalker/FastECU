#include "src/ui/desktop/definition/definition_authoring_dialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

#include "src/ui/desktop/definition/definition_header_form.h"

namespace fastecu::ui
{
namespace
{

// The legacy "No file selected!" nag: Ok re-opens the chooser, Cancel gives
// up. `noun` is "create" for a save chooser and "select" for an open one,
// matching the two legacy wordings verbatim.
bool user_wants_to_retry(QWidget *parent, const QString& noun)
{
    QDialog dialog(parent);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel("No file selected!\n\nIf you still want to " + noun +
                             " file click 'Ok'\nIf you want to continue to use ROM without definition, click 'Cancel'");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(label);
    layout->addWidget(buttons);
    return dialog.exec() != QDialog::Rejected;
}

enum class PathMode
{
    Open,
    Save
};

// The legacy chooser-plus-nag loop, identical in all three legacy call
// sites bar the chooser call and the nag's verb. Returns an empty QString
// when the user gives up.
QString select_definition_path(QWidget *parent, const QString& directory, PathMode mode)
{
    QString filename;
    bool gaveUp = false;
    while (filename.isEmpty() && !gaveUp)
    {
        filename = mode == PathMode::Save
                       ? QFileDialog::getSaveFileName(parent, QObject::tr("Select definition file"), directory,
                                                      QObject::tr("Definition file (*.xml)"))
                       : QFileDialog::getOpenFileName(parent, QObject::tr("Select definition file"), directory,
                                                      QObject::tr("Definition file (*.xml)"));
        if (filename.isEmpty() && !user_wants_to_retry(parent, mode == PathMode::Save ? "create" : "select"))
        {
            gaveUp = true;
        }
    }
    return filename;
}

// The shared "Please provide ROM Information:" modal. Returns the editors
// and whether the user accepted.
struct HeaderDialogResult
{
    bool accepted{false};
    HeaderFormEditors editors;
};

// `dialog` is supplied by the caller rather than constructed here: the
// editors in the returned result are its children, so they die with it and
// the caller reads them after this returns.
HeaderDialogResult run_header_dialog(QDialog& dialog, const QStringList& labels, const QStringList& names,
                                     const QStringList& values)
{
    HeaderDialogResult result;
    result.editors = populate_header_dialog(dialog, labels, names, values);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    dialog.layout()->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.setMinimumWidth(500);
    result.accepted = dialog.exec() == QDialog::Accepted;
    return result;
}

} // namespace

HeaderFormEditors populate_header_dialog(QDialog& dialog, const QStringList& labels, const QStringList& names,
                                         const QStringList& values)
{
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Please provide ROM Information:"));

    // build_header_form parents the editors to `grid`, which is unparented
    // until addLayout reparents its widgets onto `dialog`.
    auto *grid = new QGridLayout();
    HeaderFormEditors editors = build_header_form(grid, labels, names, values);
    layout->addLayout(grid);
    return editors;
}

void record_definition(FileActions::ConfigValuesStructure& config, const HeaderFormEditors& editors,
                       const fastecu::definition::DefinitionHeaderInput& input, const QString& filename)
{
    config.ecuflash_def_cal_id.append(QString::fromStdString(input.xml_id));
    config.ecuflash_def_cal_id_addr.append(line_edit_value(editors, "internalidaddress"));
    config.ecuflash_def_ecu_id.append(line_edit_value(editors, "ecuid"));
    config.ecuflash_def_filename.append(filename);
}

DefinitionAuthoringDialog::DefinitionAuthoringDialog(FileActions& file_actions, fastecu::IFileRepository& repository,
                                                     QWidget *parent)
    : QObject(parent), fileActions_(file_actions), repository_(repository), parent_(parent)
{
}

bool DefinitionAuthoringDialog::create_new_definition(FileActions::EcuCalDefStructure *ecuCalDef)
{
    FileActions::ConfigValuesStructure *configValues = &fileActions_.ConfigValuesStruct;

    emit LOG_D("Create header", true, true);
    // `dialog` owns the form's editors, and form.editors is read as far down
    // as the record_definition call, so it stays alive for the whole
    // function rather than living inside run_header_dialog.
    QDialog dialog(parent_);
    const HeaderDialogResult form =
        run_header_dialog(dialog, ecuCalDef->DefHeaderStrings, ecuCalDef->DefHeaderNames, {});
    if (!form.accepted)
    {
        return true;
    }

    QString filename =
        select_definition_path(parent_, configValues->ecuflash_definition_files_directory, PathMode::Save);
    if (filename.isEmpty())
    {
        return true;
    }
    filename = normalize_xml_suffix(filename);

    const auto input = definition_header_input(form.editors);
    if (!input.has_value())
    {
        emit LOG_E("Unable to create definition: " + QString::fromStdString(input.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to create definition: " + QString::fromStdString(input.error().detail));
        return false;
    }
    for (const QLineEdit *editor : form.editors.line_edits)
    {
        if (editor->objectName() != "include")
        {
            emit LOG_D(editor->text(), true, true);
        }
    }

    const fastecu::Status status = fileActions_.submit_new_definition(filename.toStdString(), *input);
    if (!status.has_value())
    {
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to open definition file for writing: " +
                                 QString::fromStdString(status.error().detail));
        return false;
    }

    record_definition(*configValues, form.editors, *input, filename);
    return true;
}

bool DefinitionAuthoringDialog::use_existing_definition(FileActions::EcuCalDefStructure *ecuCalDef)
{
    FileActions::ConfigValuesStructure *configValues = &fileActions_.ConfigValuesStruct;

    const QString source =
        select_definition_path(parent_, configValues->ecuflash_definition_files_directory, PathMode::Open);
    if (source.isEmpty())
    {
        return true;
    }

    const auto sourceContents = repository_.read(source.toStdString());
    if (!sourceContents.has_value())
    {
        emit LOG_E("Unable to import definition: " + QString::fromStdString(sourceContents.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"), "Unable to open definition file for reading");
        return false;
    }
    const QByteArray sourceBytes(reinterpret_cast<const char *>(sourceContents->data()),
                                 static_cast<qsizetype>(sourceContents->size()));
    const QStringList headerData =
        FileActions::collect_ecuflash_base_header_fields(*ecuCalDef, {QString::fromUtf8(sourceBytes)});

    // headerData is a flat (name, value, name, value, ...) list; split it
    // into the two parallel lists build_header_form expects.
    QStringList names;
    QStringList values;
    for (int i = 0; i + 1 < headerData.length(); i += 2)
    {
        names.append(headerData.at(i));
        values.append(headerData.at(i + 1));
    }

    emit LOG_D("Create header", true, true);
    // As in create_new_definition: `dialog` outlives every read of
    // form.editors below, down to the record_definition call.
    QDialog dialog(parent_);
    const HeaderDialogResult form = run_header_dialog(dialog, ecuCalDef->DefHeaderStrings, names, values);
    if (!form.accepted)
    {
        return true;
    }

    QString filename =
        select_definition_path(parent_, configValues->ecuflash_definition_files_directory, PathMode::Save);
    if (filename.isEmpty())
    {
        return true;
    }
    filename = normalize_xml_suffix(filename);

    const auto input = definition_header_input(form.editors);
    if (!input.has_value())
    {
        emit LOG_E("Unable to import definition: " + QString::fromStdString(input.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to import definition: " + QString::fromStdString(input.error().detail));
        return false;
    }
    emit LOG_D("Write to file", true, true);
    for (const QLineEdit *editor : form.editors.line_edits)
    {
        if (editor->objectName() != "include")
        {
            emit LOG_D(editor->text(), true, true);
        }
    }

    const fastecu::Status status =
        fileActions_.submit_imported_definition(source.toStdString(), filename.toStdString(), *input);
    if (!status.has_value())
    {
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to open definition file for writing: " +
                                 QString::fromStdString(status.error().detail));
        return false;
    }

    record_definition(*configValues, form.editors, *input, filename);
    return true;
}

} // namespace fastecu::ui
