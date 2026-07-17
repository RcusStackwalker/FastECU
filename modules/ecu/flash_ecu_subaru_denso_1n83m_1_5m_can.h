#ifndef FLASH_ECU_SUBARU_DENSO_1N83M_1_5M_CAN_H
#define FLASH_ECU_SUBARU_DENSO_1N83M_1_5M_CAN_H

#include <memory>

#include <QEventLoop>
#include <QWidget>

#include <file_actions.h>
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;
class FlashEcuSubaruDenso1N83M_1_5MCanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Subaru Denso 1N83M/1.5M CAN reflash module. GUI-thread half of the
// dialog+operation split (worker-thread migration);
// protocol logic lives in FlashEcuSubaruDenso1N83M_1_5MCanOperation.
class FlashEcuSubaruDenso1N83M_1_5MCan : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashEcuSubaruDenso1N83M_1_5MCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent = nullptr);
    ~FlashEcuSubaruDenso1N83M_1_5MCan();

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
    FlashEcuSubaruDenso1N83M_1_5MCanOperation *m_operation = nullptr;

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // FLASH_ECU_SUBARU_DENSO_1N83M_1_5M_CAN_H
