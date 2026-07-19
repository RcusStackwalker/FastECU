#ifndef FLASH_ECU_UNBRICK_SUBARU_DENSO_MC68HC16Y5_02_H
#define FLASH_ECU_UNBRICK_SUBARU_DENSO_MC68HC16Y5_02_H

#include <memory>

#include <QEventLoop>
#include <QWidget>

#include "src/backend/definitions/file_actions.h"
#include <ui_ecu_operations.h>

// Forward declaration
class SerialPortActions;
class FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

class FlashEcuSubaruDensoMC68HC16Y5_02_BDM : public QDialog
{
    Q_OBJECT

  public:
    explicit FlashEcuSubaruDensoMC68HC16Y5_02_BDM(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent = nullptr);
    ~FlashEcuSubaruDensoMC68HC16Y5_02_BDM();

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
    FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation *m_operation = nullptr;

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};

#endif // FLASH_ECU_UNBRICK_SUBARU_DENSO_MC68HC16Y5_02_H
