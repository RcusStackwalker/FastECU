#pragma once
#include <QDialog>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "src/backend/config/config_session.h"
#include "src/backend/definition/definition_writer.h"
#include "src/platform/desktop/common/definition/definition_catalog_session.h"
#include "src/backend/ports/file_repository.h"
#include "src/ui/desktop/definition/definition_header_form.h"

namespace fastecu::ui
{

// Fills `dialog` with the "Please provide ROM Information:" form and returns
// the editors it created. The editors are `dialog`'s children through Qt's
// parent chain, so they are readable exactly as long as the caller keeps
// `dialog` alive -- a dialog destroyed before the editors are read takes
// them with it.
HeaderFormEditors populate_header_dialog(QDialog& dialog, const QStringList& labels, const QStringList& names,
                                         const QStringList& values);

// The two interactive definition-authoring wizards, moved out of
// FileActions. Each collects ROM
// header fields in a modal form, asks for a destination path, and hands the
// result to the composition-owned catalog session.
//
// Dialogs are parented to the QWidget passed in. The legacy
// `new QDialog(this)` leaked one dialog per invocation.
// The EcuFlash definition header fields the authoring forms edit, in form
// order: display labels and the matching <romid>/<rom> element names.
QStringList definition_header_labels();
QStringList definition_header_names();

class DefinitionAuthoringDialog : public QObject
{
    Q_OBJECT

  public:
    DefinitionAuthoringDialog(fastecu::desktop::definition::DefinitionCatalogSession& catalogs,
                              const fastecu::config::ConfigSession& config, fastecu::IFileRepository& repository,
                              QWidget *parent = nullptr);

    // Both return true when the ROM may continue to be used -- including
    // when the user cancels out, which legacy signalled by returning
    // ecuCalDef unchanged. false means a genuine failure (legacy nullptr).
    bool create_new_definition();
    bool use_existing_definition();

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  private:
    fastecu::desktop::definition::DefinitionCatalogSession& catalogs_;
    const fastecu::config::ConfigSession& config_;
    fastecu::IFileRepository& repository_;
    QWidget *parent_;
};

} // namespace fastecu::ui
