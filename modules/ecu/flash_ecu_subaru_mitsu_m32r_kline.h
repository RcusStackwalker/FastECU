#ifndef FLASH_ECU_MITSU_KLINE_H
#define FLASH_ECU_MITSU_KLINE_H

#include <memory>

#include <QEventLoop>
#include <QWidget>

#include <file_actions.h>
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;
class FlashEcuSubaruMitsuM32rKlineOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

class FlashEcuSubaruMitsuM32rKline : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashEcuSubaruMitsuM32rKline(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent = nullptr);
    ~FlashEcuSubaruMitsuM32rKline();

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
    FlashEcuSubaruMitsuM32rKlineOperation *m_operation = nullptr;

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // FLASH_ECU_MITSU_KLINE_H
