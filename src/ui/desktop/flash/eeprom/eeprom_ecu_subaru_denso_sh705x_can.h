#ifndef EEPROM_ECU_SUBARU_DENSO_SH705X_CAN_H
#define EEPROM_ECU_SUBARU_DENSO_SH705X_CAN_H

#include <memory>

#include <QDialog>
#include <QEventLoop>
#include <QString>
#include <QWidget>

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
// (2 -> 3 -> 4, step 5c Task 17). CAN sibling of EepromEcuSubaruDensoSH705xKline
// (this package) -- identical structure, DensoSh705xEepromCanExecutor over a
// DesktopCanFlashTransport instead of the K-Line pair. See that class's
// header comment for the full rationale (mode orchestration, ownership of
// `serial`, and the test-seam design).
class EepromEcuSubaruDensoSH705xCan : public QDialog
{
    Q_OBJECT

  public:
    EepromEcuSubaruDensoSH705xCan(SerialPortActions *serial,
                                  FileActions::EcuCalDefStructure *ecuCalDef,
                                  const QString& cmd_type, QWidget *parent = nullptr);
    ~EepromEcuSubaruDensoSH705xCan() override;

    void run();

  signals:
    void external_logger(QString message);
    void external_logger(int value);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  protected:
    // Test seams (step 5c, Task 17) -- see
    // EepromEcuSubaruDensoSH705xKline.h's identical seam for the rationale.
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
    QString cmd_type_;
    fastecu::flash::EepromReadMode currentMode_ = fastecu::flash::EepromReadMode::Mode2;
    std::unique_ptr<fastecu::flash::FlashWorker> worker_;
    QEventLoop *loop_ = nullptr;

    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // EEPROM_ECU_SUBARU_DENSO_SH705X_CAN_H
