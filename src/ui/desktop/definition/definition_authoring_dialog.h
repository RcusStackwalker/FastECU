#pragma once
#include <QDialog>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "src/backend/definition/definition_writer.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/file_repository.h"
#include "src/ui/desktop/definition/definition_header_form.h"

namespace fastecu::ui
{

// The two non-modal seams of the wizards below. Both are shared by
// create_new_definition and use_existing_definition, and both are declared
// here rather than kept file-local so they can be exercised without driving
// a modal dialog.

// Fills `dialog` with the "Please provide ROM Information:" form and returns
// the editors it created. The editors are `dialog`'s children through Qt's
// parent chain, so they are readable exactly as long as the caller keeps
// `dialog` alive -- a dialog destroyed before the editors are read takes
// them with it.
HeaderFormEditors populate_header_dialog(QDialog& dialog, const QStringList& labels, const QStringList& names,
                                         const QStringList& values);

// Appends one entry to each of the four ecuflash_def_* config lists, taking
// the calibration ID from `input` and the ID address and ECU ID from the
// form's editors. Called only once the definition has actually been written,
// so a failed write leaves the lists untouched.
void record_definition(FileActions::ConfigValuesStructure& config, const HeaderFormEditors& editors,
                       const fastecu::definition::DefinitionHeaderInput& input, const QString& filename);

// The two interactive definition-authoring wizards, moved out of
// FileActions (file_actions.cpp:854 and :979 at f74c525). Each collects ROM
// header fields in a modal form, asks for a destination path, and hands the
// result to FileActions's portable submit_*_definition seam.
//
// Dialogs are parented to the QWidget passed in, not to FileActions: 6a-3
// removes QWidget from FileActions entirely, and the legacy
// `new QDialog(this)` leaked one dialog per invocation.
class DefinitionAuthoringDialog : public QObject
{
    Q_OBJECT

  public:
    DefinitionAuthoringDialog(FileActions& file_actions, fastecu::IFileRepository& repository,
                              QWidget *parent = nullptr);

    // Both return true when the ROM may continue to be used -- including
    // when the user cancels out, which legacy signalled by returning
    // ecuCalDef unchanged. false means a genuine failure (legacy nullptr).
    bool create_new_definition(FileActions::EcuCalDefStructure *ecuCalDef);
    bool use_existing_definition(FileActions::EcuCalDefStructure *ecuCalDef);

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  private:
    FileActions& fileActions_;
    fastecu::IFileRepository& repository_;
    QWidget *parent_;
};

} // namespace fastecu::ui
