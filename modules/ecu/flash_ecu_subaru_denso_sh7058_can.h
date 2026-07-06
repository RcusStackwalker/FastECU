#ifndef FLASH_ECU_SUBARU_DENSO_SH7058_CAN_H
#define FLASH_ECU_SUBARU_DENSO_SH7058_CAN_H

#include <QEventLoop>
#include <QWidget>

#include <file_actions.h>
#include <ui_ecu_operations.h>

//Forward declaration
class SerialPortActions;
class FlashEcuSubaruDensoSH7058CanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Subaru Denso SH7058 32-bit CAN (iso15765) reflash module. GUI-thread half
// of the dialog+operation split (worker-thread migration);
// protocol logic lives in FlashEcuSubaruDensoSH7058CanOperation.
class FlashEcuSubaruDensoSH7058Can : public QDialog
{
    Q_OBJECT

public:
    explicit FlashEcuSubaruDensoSH7058Can(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent = nullptr);
    ~FlashEcuSubaruDensoSH7058Can();

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
    Ui::EcuOperationsWindow *ui;
    FlashEcuSubaruDensoSH7058CanOperation *m_operation = nullptr;

};

#endif // FLASH_ECU_SUBARU_DENSO_SH7058_CAN_H
