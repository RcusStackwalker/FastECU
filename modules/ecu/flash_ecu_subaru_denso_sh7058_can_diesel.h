#ifndef FLASH_ECU_SUBARU_DENSO_SH7058_CAN_DIESEL_H
#define FLASH_ECU_SUBARU_DENSO_SH7058_CAN_DIESEL_H

#include <memory>

#include <QEventLoop>
#include <QWidget>

#include <file_actions.h>
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;
class FlashEcuSubaruDensoSH7058CanDieselOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Subaru 07+ Diesel SH7058 32-bit CAN (iso15765) reflash module. GUI-thread
// half of the dialog+operation split (worker-thread migration);
// protocol logic lives in FlashEcuSubaruDensoSH7058CanDieselOperation.
class FlashEcuSubaruDensoSH7058CanDiesel : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashEcuSubaruDensoSH7058CanDiesel(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent = nullptr);
    ~FlashEcuSubaruDensoSH7058CanDiesel();

    void run();

  signals:
    void external_logger(QString message);
    void external_logger(int value);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  private:
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;

    void closeEvent(QCloseEvent *event);
    void set_progressbar_value(int value);

    SerialPortActions *serial;
    FlashEcuSubaruDensoSH7058CanDieselOperation *m_operation = nullptr;

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // FLASH_ECU_SUBARU_DENSO_SH7058_CAN_DIESEL_H
