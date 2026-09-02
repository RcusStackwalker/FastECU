#pragma once
#include <QObject>
#include <QString>
#include <QWidget>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/file_repository.h"

namespace fastecu::ui
{

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
