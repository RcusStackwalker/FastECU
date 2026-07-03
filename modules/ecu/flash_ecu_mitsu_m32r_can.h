#ifndef FLASH_ECU_MITSU_M32R_CAN_H
#define FLASH_ECU_MITSU_M32R_CAN_H

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QMainWindow>
#include <QMessageBox>
#include <QSerialPort>
#include <QTime>
#include <QTimer>
#include <QWidget>

#include <kernelmemorymodels.h>
#include <file_actions.h>
#include <ui_ecu_operations.h>

class SerialPortActions;
class FlashEcuMitsuM32rCanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash module. GUI-thread
// half of the dialog+operation split (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md);
// protocol logic lives in FlashEcuMitsuM32rCanOperation.
class FlashEcuMitsuM32rCan : public QDialog
{
    Q_OBJECT

public:
    explicit FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent = nullptr);
    ~FlashEcuMitsuM32rCan();

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
    FlashEcuMitsuM32rCanOperation *m_operation = nullptr;
};

#endif // FLASH_ECU_MITSU_M32R_CAN_H
