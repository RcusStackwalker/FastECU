#ifndef FLASH_TCU_MITSU_MH8104_CAN_H
#define FLASH_TCU_MITSU_MH8104_CAN_H

#include <memory>

#include <QEventLoop>
#include <QWidget>

#include "src/backend/definitions/file_actions.h"
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;
class FlashTcuCvtSubaruMitsuMH8104CanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

class FlashTcuCvtSubaruMitsuMH8104Can : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashTcuCvtSubaruMitsuMH8104Can(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent = nullptr);
    ~FlashTcuCvtSubaruMitsuMH8104Can();

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
    int send_log_window_message(QString message, bool timestamp, bool linefeed);
    void set_progressbar_value(int value);

    SerialPortActions *serial;
    FlashTcuCvtSubaruMitsuMH8104CanOperation *m_operation = nullptr;

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // FLASH_TCU_MITSU_MH8104_CAN_H
