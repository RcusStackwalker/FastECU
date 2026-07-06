#ifndef FLASH_ECU_SUBARU_HITACHI_M32R_CAN_H
#define FLASH_ECU_SUBARU_HITACHI_M32R_CAN_H

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QMainWindow>
#include <QSerialPort>
#include <QTime>
#include <QTimer>
#include <QWidget>

#include <kernelmemorymodels.h>
#include <file_actions.h>
#include <ui_ecu_operations.h>

//Forward declaration
class SerialPortActions;
class FlashEcuSubaruHitachiM32rCanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class EcuOperationsWindow;
}
QT_END_NAMESPACE

class FlashEcuSubaruHitachiM32rCan : public QDialog
{
    Q_OBJECT

public:
    explicit FlashEcuSubaruHitachiM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent);
    ~FlashEcuSubaruHitachiM32rCan();

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
    FlashEcuSubaruHitachiM32rCanOperation *m_operation = nullptr;

};

#endif // FLASH_ECU_SUBARU_HITACHI_M32R_CAN_H
