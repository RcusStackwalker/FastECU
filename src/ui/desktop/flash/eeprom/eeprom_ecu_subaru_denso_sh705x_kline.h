#pragma once

#include <memory>

#include <QDialog>
#include <QEventLoop>
#include <QString>
#include <QWidget>

#include "src/backend/config/config_paths.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/flash/flash_worker.h"
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Orchestrates one bounded FlashWorker attempt per EEPROM read mode
// (2 -> 3 -> 4, step 5c Task 17). Each attempt is a fully independent
// FlashWorker run, joined to completion before the next confirmation dialog
// is shown or the next attempt starts -- never two attempts, and never a
// human confirmation, overlapping a single worker's lifetime. Replaces the
// legacy EepromEcuSubaruDensoSH705xKlineOperation (a FlashOperationWorker
// subclass that looped mode 2..4 internally, deleted by an earlier task in
// this plan); the mode loop now lives here, one FlashPlan/FlashWorker per
// attempt, built via the portable EEPROM read-plan use case and executed via
// DensoSh705xEepromKlineExecutor over a DesktopKlineFlashTransport.
//
// Ownership note: the transport is constructed from the non-owning
// SerialPortActions* overload (DesktopKlineFlashTransport(SerialPortActions*),
// step 5c Task 17), NOT the owning std::unique_ptr<SerialPortActions>
// overload. `serial` is MainWindow's single, session-lifetime connection
// object (constructed once in mainwindow.cpp and reused for the entire app
// session, across every read/write/flash/logging operation) -- the owning
// constructor's close() would destroy it the moment any one attempt here
// finishes, breaking every other part of the application for the rest of
// the session. See DesktopKlineFlashTransport.h's non-owning constructor
// comment for the full rationale.
class EepromEcuSubaruDensoSH705xKline : public QDialog
{
    Q_OBJECT

  public:
    EepromEcuSubaruDensoSH705xKline(SerialPortActions *serial,
                                    FileActions::EcuCalDefStructure *ecuCalDef,
                                    const fastecu::config::ConfigPaths& paths,
                                    const QString& cmd_type, QWidget *parent = nullptr);
    ~EepromEcuSubaruDensoSH705xKline() override;

    void run();

  signals:
    void external_logger(QString message);
    void external_logger(int value);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  protected:
    // Test seams (step 5c, Task 17). The dialog's own public constructor
    // signature is unchanged -- MainWindow's EEPROM dispatch (mainwindow.cpp)
    // does not need to change -- so a test subclass overrides these
    // virtuals instead of injecting fakes through the constructor (the same
    // idea as FlashOperationWorker's injectable PromptFn, generalized to
    // also cover plan building and worker construction). Every one of these
    // has a real, hardware/dialog-driving default implementation defined in
    // the .cpp; only tests override them.
    virtual fastecu::Result<fastecu::flash::FlashPlan> buildPlan(
        fastecu::flash::EepromReadMode mode);
    virtual std::unique_ptr<fastecu::flash::FlashWorker> makeWorker(
        fastecu::flash::FlashPlan plan);
    virtual int confirm(const QString& title, const QString& text, int buttons,
                        int defaultButton);
    virtual void showFailureDialog(fastecu::ErrorKind kind, const QString& detail);

  private:
    void runAttempt(fastecu::flash::EepromReadMode mode);
    void onWorkerFinished(fastecu::flash::FlashWorkerResult result);
    void closeEvent(QCloseEvent *event) override;
    void set_progressbar_value(int value);

    SerialPortActions *serial_;
    FileActions::EcuCalDefStructure *ecuCalDef_;
    // Copied, not borrowed: MainWindow's ConfigValuesStructure can be
    // repopulated (read_config_file/read_protocols_file rewrite it in place)
    // while this modal dialog is open.
    fastecu::config::ConfigPaths paths_;
    QString cmd_type_;
    fastecu::flash::EepromReadMode currentMode_ = fastecu::flash::EepromReadMode::Mode2;
    std::unique_ptr<fastecu::flash::FlashWorker> worker_;
    QEventLoop *loop_ = nullptr;

    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};
