#include "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can.h"

#include <utility>
#include "src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.h"
#include "serial_port_actions.h"

EepromEcuSubaruDensoSH705xCan::EepromEcuSubaruDensoSH705xCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent)
    : QDialog(parent), ecuCalDef(ecuCalDef), cmd_type(cmd_type), ui{std::make_unique<Ui::EcuOperationsWindow>()}
{
    ui->setupUi(this);

    if (cmd_type == "test_write")
    {
        this->setWindowTitle("Test write ROM " + ecuCalDef->FileName + " to ECU");
    }
    else if (cmd_type == "write")
    {
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to ECU");
    }
    else if (cmd_type == "read")
    {
        this->setWindowTitle("Read ROM from ECU");
    }

    this->serial = serial;
}

void EepromEcuSubaruDensoSH705xCan::run()
{
    this->show();
    set_progressbar_value(0);

    bool success = false;

    int ret = QMessageBox::information(this, tr("Connecting to ECU"),
                                       tr("Downloading EEPROM content. There is 3 different option depends on "
                                          "ECU. All 3 option shows content on screen and you can save it when "
                                          "it looks ok.\n\n"
                                          "Turn ignition ON and press OK to start initializing connection to ECU"),
                                       QMessageBox::Ok | QMessageBox::Cancel,
                                       QMessageBox::Ok);

    switch (ret)
    {
    case QMessageBox::Ok:
    {
        m_operation = new EepromEcuSubaruDensoSH705xCanOperation(serial, ecuCalDef, cmd_type, this);
        connect(m_operation, &FlashOperationWorker::LOG_E, this, &EepromEcuSubaruDensoSH705xCan::LOG_E);
        connect(m_operation, &FlashOperationWorker::LOG_W, this, &EepromEcuSubaruDensoSH705xCan::LOG_W);
        connect(m_operation, &FlashOperationWorker::LOG_I, this, &EepromEcuSubaruDensoSH705xCan::LOG_I);
        connect(m_operation, &FlashOperationWorker::LOG_D, this, &EepromEcuSubaruDensoSH705xCan::LOG_D);
        connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                this, [this](QString msg)
                { emit external_logger(std::move(msg)); });
        connect(m_operation, &FlashOperationWorker::progressChanged,
                this, &EepromEcuSubaruDensoSH705xCan::set_progressbar_value);

        QEventLoop loop;
        connect(m_operation, &FlashOperationWorker::operationFinished, &loop,
                [&success, &loop](bool ok)
                { success = ok; loop.quit(); });

        m_operation->start();
        loop.exec();
        m_operation->wait();
        delete m_operation;
        m_operation = nullptr;
    }
    case QMessageBox::Cancel:
        emit LOG_D("Operation canceled", true, true);
        this->close();
        break;
    default:
        QMessageBox::warning(this, tr("Connecting to ECU"), "Unknown operation selected!");
        emit LOG_D("Unknown operation selected!", true, true);
        this->close();
        break;
    }
    if (!success)
    {
        QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, press OK to exit and try again");
    }
}

EepromEcuSubaruDensoSH705xCan::~EepromEcuSubaruDensoSH705xCan()
{
}

void EepromEcuSubaruDensoSH705xCan::closeEvent(QCloseEvent *bar)
{
    if (m_operation)
    {
        m_operation->requestStop();
    }
}

void EepromEcuSubaruDensoSH705xCan::set_progressbar_value(int value)
{
    bool valueChanged = true;
    if (ui->progressbar)
    {
        valueChanged = ui->progressbar->value() != value;
        ui->progressbar->setValue(value);
    }
    if (valueChanged)
    {
        emit external_logger(value);
    }
}
