#ifndef EEPROM_ECU_SUBARU_DENSO_SH705X_CAN_H
#define EEPROM_ECU_SUBARU_DENSO_SH705X_CAN_H

#include <QEventLoop>
#include <QWidget>

#include <file_actions.h>
#include <ui_ecu_operations.h>

//Forward declaration
class SerialPortActions;
class EepromEcuSubaruDensoSH705xCanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class EcuOperationsWindow;
}
QT_END_NAMESPACE

class EepromEcuSubaruDensoSH705xCan : public QDialog
{
    Q_OBJECT

public:
    EepromEcuSubaruDensoSH705xCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent = nullptr);
    ~EepromEcuSubaruDensoSH705xCan();

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

    void closeEvent(QCloseEvent *bar);
    void set_progressbar_value(int value);

    SerialPortActions *serial;
    Ui::EcuOperationsWindow *ui;
    EepromEcuSubaruDensoSH705xCanOperation *m_operation = nullptr;

};


#endif // EEPROM_ECU_SUBARU_DENSO_SH705X_CAN_H
