#ifndef FLASH_ECU_SUBARU_UNISIA_JECS_M32R_BOOT_MODE_H
#define FLASH_ECU_SUBARU_UNISIA_JECS_M32R_BOOT_MODE_H

#include <QEventLoop>
#include <QWidget>

#include <file_actions.h>
#include <ui_ecu_operations.h>

//Forward declaration
class SerialPortActions;
class FlashEcuSubaruUnisiaJecsM32rBootModeOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

class FlashEcuSubaruUnisiaJecsM32rBootMode : public QDialog
{
    Q_OBJECT

public:
    explicit FlashEcuSubaruUnisiaJecsM32rBootMode(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent);
    ~FlashEcuSubaruUnisiaJecsM32rBootMode();

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
    FlashEcuSubaruUnisiaJecsM32rBootModeOperation *m_operation = nullptr;

};

#endif // FLASH_ECU_SUBARU_UNISIA_JECS_M32R_BOOT_MODE_H
