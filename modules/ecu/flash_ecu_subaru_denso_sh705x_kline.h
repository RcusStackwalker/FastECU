#ifndef FLASH_ECU_SUBARU_DENSO_SH705X_KLINE_H
#define FLASH_ECU_SUBARU_DENSO_SH705X_KLINE_H

#include <memory>

#include <QEventLoop>
#include <QWidget>

#include "src/backend/definitions/file_actions.h"
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;
class FlashEcuSubaruDensoSH705xKlineOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

class FlashEcuSubaruDensoSH705xKline : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashEcuSubaruDensoSH705xKline(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent = nullptr);
    ~FlashEcuSubaruDensoSH705xKline();

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
    FlashEcuSubaruDensoSH705xKlineOperation *m_operation = nullptr;

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // FLASH_ECU_SUBARU_DENSO_SH705X_KLINE_H
