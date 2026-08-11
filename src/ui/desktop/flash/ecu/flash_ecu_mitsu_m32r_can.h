#pragma once

#include <memory>

#include <QDialog>
#include <QEventLoop>
#include <QString>
#include <QWidget>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/flash/flash_worker.h"
#include <ui_ecu_operations.h>

class SerialPortActions;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash module. Desktop half
// of the portable split (step 5 tail, wave 0): this dialog collects every
// operator confirmation, builds a FlashPlan through
// build_mitsu_colt_m32r_can_plan(), and runs exactly one
// MitsuColtM32rCanExecutor attempt on a FlashWorker. Same shape as the
// EEPROM dialogs in src/ui/desktop/flash/eeprom.
//
// Every write gate declared by the plan is answered before the worker starts,
// because a synchronous dialog-free executor cannot block mid-run for a human
// answer; a confirmation's presence on the plan means "granted".
class FlashEcuMitsuM32rCan : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent = nullptr);
    ~FlashEcuMitsuM32rCan();

    void run();

  signals:
    void external_logger(QString message);
    void external_logger(int value);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  protected:
    // Test seams, same set and same rationale as the EEPROM pair's dialogs
    // (eeprom_ecu_subaru_denso_sh705x_can.h): the dialog test drives the real
    // run()/onWorkerFinished() orchestration against a scripted executor and
    // a fake clock, answers prompts from a script, and records dialogs
    // instead of showing modals -- no hardware, no blocking QMessageBox.
    virtual fastecu::Result<fastecu::flash::FlashPlan> buildPlan();
    virtual std::unique_ptr<fastecu::flash::FlashWorker> makeWorker(
        fastecu::flash::FlashPlan plan);
    virtual int confirm(const QString& title, const QString& text, int buttons,
                        int defaultButton);
    // Separate from confirm(): the success notification is an information
    // box with no choice to make, and it must be overridable on its own --
    // it is the last thing between the FullRomData copy and run() returning.
    virtual void showSuccessDialog();
    virtual void showFailureDialog(fastecu::ErrorKind kind, const QString& detail);

  private:
    void onWorkerFinished(fastecu::flash::FlashWorkerResult result);

    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
    // Borrowed, never owned: MainWindow's single session-lifetime
    // SerialPortActions. See makeWorker() for the port-lifetime contract.
    SerialPortActions *serial;
    std::unique_ptr<fastecu::flash::FlashWorker> worker_;
    QEventLoop *loop_ = nullptr;

    void closeEvent(QCloseEvent *event) override;
    void set_progressbar_value(int value);

    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};
