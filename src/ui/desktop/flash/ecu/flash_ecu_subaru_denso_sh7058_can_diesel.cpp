#include "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.h"

#include <utility>
#include "src/backend/flash/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

FlashEcuSubaruDensoSH7058CanDiesel::FlashEcuSubaruDensoSH7058CanDiesel(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent)
    : QDialog(parent), ecuCalDef(ecuCalDef), cmd_type(cmd_type), ui{std::make_unique<Ui::EcuOperationsWindow>()}
{
    // EURO4 0xFFFF4000 0x3000
    // EURO5 0xFFFEE000 0x2000

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

FlashEcuSubaruDensoSH7058CanDiesel::~FlashEcuSubaruDensoSH7058CanDiesel()
{
}

void FlashEcuSubaruDensoSH7058CanDiesel::closeEvent(QCloseEvent *event)
{
    if (m_operation)
    {
        m_operation->requestStop();
    }
}

void FlashEcuSubaruDensoSH7058CanDiesel::run()
{
    this->show();

    set_progressbar_value(0);

    int ret = QMessageBox::warning(this, tr("Connecting to ECU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to ECU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
    case QMessageBox::Ok:
    {
        m_operation = new FlashEcuSubaruDensoSH7058CanDieselOperation(serial, ecuCalDef, cmd_type, this);
        connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuSubaruDensoSH7058CanDiesel::LOG_E);
        connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuSubaruDensoSH7058CanDiesel::LOG_W);
        connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuSubaruDensoSH7058CanDiesel::LOG_I);
        connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuSubaruDensoSH7058CanDiesel::LOG_D);
        connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                this, [this](QString msg)
                { emit external_logger(std::move(msg)); });
        connect(m_operation, &FlashOperationWorker::progressChanged,
                this, &FlashEcuSubaruDensoSH7058CanDiesel::set_progressbar_value);

        QEventLoop loop;
        bool success = false;
        connect(m_operation, &FlashOperationWorker::operationFinished, &loop,
                [&success, &loop](bool ok)
                { success = ok; loop.quit(); });

        m_operation->start();
        loop.exec();
        m_operation->wait();
        delete m_operation;
        m_operation = nullptr;

        emit external_logger("Finished");

        if (success)
        {
            QMessageBox::information(this, tr("ECU Operation"), "ECU operation was succesful, press OK to exit");
            this->close();
        }
        else
        {
            QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, press OK to exit and try again");
        }
        break;
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
}

void FlashEcuSubaruDensoSH7058CanDiesel::set_progressbar_value(int value)
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
