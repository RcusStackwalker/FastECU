#include "src/ui/desktop/definition/dialog/definition_authoring_dialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

#include "src/ui/desktop/config_fields.h"
#include "src/ui/desktop/definition/definition_header_form.h"

namespace fastecu::ui
{
namespace
{

// The legacy "No file selected!" nag: Ok re-opens the chooser, Cancel gives
// up. `noun` is "create" for a save chooser and "select" for an open one,
// matching the two legacy wordings verbatim.
bool userWantsToRetry(QWidget *parent, const QString& noun)
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
    kOpen,
    kSave
};

// The legacy chooser-plus-nag loop, identical in all three legacy call
// sites bar the chooser call and the nag's verb. Returns an empty QString
// when the user gives up.
QString selectDefinitionPath(QWidget *parent, const QString& directory, PathMode mode)
{
    QString filename;
    bool gaveUp = false;
    while (filename.isEmpty() && !gaveUp)
    {
        filename = mode == PathMode::kSave
                       ? QFileDialog::getSaveFileName(parent, QObject::tr("Select definition file"), directory,
                                                      QObject::tr("Definition file (*.xml)"))
                       : QFileDialog::getOpenFileName(parent, QObject::tr("Select definition file"), directory,
                                                      QObject::tr("Definition file (*.xml)"));
        if (filename.isEmpty() && !userWantsToRetry(parent, mode == PathMode::kSave ? "create" : "select"))
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
HeaderDialogResult runHeaderDialog(QDialog& dialog, const definition::DefinitionHeaderDraft& draft = {})
{
    HeaderDialogResult result;
    result.editors = populateHeaderDialog(dialog, draft);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    dialog.layout()->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.setMinimumWidth(500);
    result.accepted = dialog.exec() == QDialog::Accepted;
    return result;
}

} // namespace

HeaderFormEditors populateHeaderDialog(QDialog& dialog, const definition::DefinitionHeaderDraft& draft)
{
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Please provide ROM Information:"));

    // build_header_form parents the editors to `grid`, which is unparented
    // until addLayout reparents its widgets onto `dialog`.
    auto *grid = new QGridLayout();
    HeaderFormEditors editors = buildHeaderForm(grid, draft);
    layout->addLayout(grid);
    return editors;
}

DefinitionAuthoringDialog::DefinitionAuthoringDialog(fastecu::definition::DefinitionCatalogSession& catalogs,
                                                     const fastecu::config::ConfigSession& config,
                                                     fastecu::IFileRepository& repository, QWidget *parent)
    : QObject(parent), catalogs_(catalogs), config_(config), repository_(repository), parent_(parent)
{
}

void DefinitionAuthoringDialog::logHeader(const HeaderFormEditors& editors)
{
    const auto draft = readHeaderForm(editors);
    for (const auto *value :
         {&draft.xml_id, &draft.internal_id_address_text, &draft.internal_id, &draft.ecu_id, &draft.metadata.make,
          &draft.metadata.market, &draft.metadata.model, &draft.metadata.submodel, &draft.metadata.transmission,
          &draft.metadata.year, &draft.metadata.flash_method, &draft.metadata.memory_model,
          &draft.metadata.checksum_module})
    {
        emit logD(QString::fromStdString(*value), true, true);
    }
}

bool DefinitionAuthoringDialog::createNewDefinition()
{
    emit logD("Create header", true, true);
    // `dialog` owns the form's editors, and form.editors is read as far down
    // as the submission, so it stays alive for the whole
    // function rather than living inside run_header_dialog.
    QDialog dialog(parent_);
    const HeaderDialogResult form = runHeaderDialog(dialog);
    if (!form.accepted)
    {
        return true;
    }

    QString filename =
        selectDefinitionPath(parent_, qs(config_.Settings().ecuflash_definition_files_directory), PathMode::kSave);
    if (filename.isEmpty())
    {
        return true;
    }
    filename = normalizeXmlSuffix(filename);

    const auto input = definitionHeaderInput(form.editors);
    if (!input.has_value())
    {
        emit logE("Unable to create definition: " + QString::fromStdString(input.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to create definition: " + QString::fromStdString(input.error().detail));
        return false;
    }
    logHeader(form.editors);

    const fastecu::Status status =
        catalogs_.SubmitNewDefinition(filename.toStdString(), *input, /*allow_overwrite=*/true);
    if (!status.has_value())
    {
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to open definition file for writing: " +
                                 QString::fromStdString(status.error().detail));
        return false;
    }

    return true;
}

bool DefinitionAuthoringDialog::useExistingDefinition()
{
    const QString source =
        selectDefinitionPath(parent_, qs(config_.Settings().ecuflash_definition_files_directory), PathMode::kOpen);
    if (source.isEmpty())
    {
        return true;
    }

    const auto sourceContents = repository_.Read(source.toStdString());
    if (!sourceContents.has_value())
    {
        emit logE("Unable to import definition: " + QString::fromStdString(sourceContents.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"), "Unable to open definition file for reading");
        return false;
    }
    const auto draft = definition::ReadDefinitionHeader(*sourceContents);
    if (!draft.has_value())
    {
        const auto detail = "Unable to import definition: " + QString::fromStdString(draft.error().detail);
        emit logE(detail, true, true);
        QMessageBox::warning(parent_, tr("Definition file"), detail);
        return false;
    }

    emit logD("Create header", true, true);
    // As in create_new_definition: `dialog` outlives every read of
    // form.editors below, down to the submission.
    QDialog dialog(parent_);
    const HeaderDialogResult form = runHeaderDialog(dialog, *draft);
    if (!form.accepted)
    {
        return true;
    }

    QString filename =
        selectDefinitionPath(parent_, qs(config_.Settings().ecuflash_definition_files_directory), PathMode::kSave);
    if (filename.isEmpty())
    {
        return true;
    }
    filename = normalizeXmlSuffix(filename);

    const auto input = definitionHeaderInput(form.editors);
    if (!input.has_value())
    {
        emit logE("Unable to import definition: " + QString::fromStdString(input.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to import definition: " + QString::fromStdString(input.error().detail));
        return false;
    }
    emit logD("Write to file", true, true);
    logHeader(form.editors);

    const fastecu::Status status =
        catalogs_.SubmitImportedDefinition(source.toStdString(), filename.toStdString(), *input);
    if (!status.has_value())
    {
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to open definition file for writing: " +
                                 QString::fromStdString(status.error().detail));
        return false;
    }

    return true;
}

} // namespace fastecu::ui
