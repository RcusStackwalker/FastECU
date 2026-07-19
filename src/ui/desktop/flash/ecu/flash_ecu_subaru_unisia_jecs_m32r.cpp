#include "src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs_m32r.h"

#include <utility>
#include "src/backend/flash/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.h"
#include "serial_port_actions.h"

FlashEcuSubaruUnisiaJecsM32r::FlashEcuSubaruUnisiaJecsM32r(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent)
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

void FlashEcuSubaruUnisiaJecsM32r::run()
{
    this->show();

    int ret = QMessageBox::warning(this, tr("Connecting to ECU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to ECU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
    case QMessageBox::Ok:
    {
        // vBattTimer->stop();

        m_operation = new FlashEcuSubaruUnisiaJecsM32rOperation(serial, ecuCalDef, cmd_type, this);
        connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuSubaruUnisiaJecsM32r::LOG_E);
        connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuSubaruUnisiaJecsM32r::LOG_W);
        connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuSubaruUnisiaJecsM32r::LOG_I);
        connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuSubaruUnisiaJecsM32r::LOG_D);
        connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                this, [this](QString msg)
                { emit external_logger(std::move(msg)); });
        connect(m_operation, &FlashOperationWorker::progressChanged,
                this, &FlashEcuSubaruUnisiaJecsM32r::set_progressbar_value);

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
            if (!serial->get_use_openport2_adapter())
            {
                QMessageBox::information(this, tr("Programming voltage"), "Remove VPP voltage from ECU and press OK to exit");
            }
            else
            {
                QMessageBox::information(this, tr("ECU Operation"), "ECU operation was succesful, press OK to exit");
            }
            this->close();
        }
        else
        {
            if (cmd_type == "write")
            {
                QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed! Don't power off your ECU, kernel is still running and you can try flashing again!");
                emit LOG_E("*** ERROR IN FLASH PROCESS ***", true, true);
                emit LOG_E("Don't power off your ECU, kernel is still running and you can try flashing again!", true, true);
            }
            else
            {
                QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, press OK to exit and try again");
            }
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

FlashEcuSubaruUnisiaJecsM32r::~FlashEcuSubaruUnisiaJecsM32r()
{
}

void FlashEcuSubaruUnisiaJecsM32r::closeEvent(QCloseEvent *event)
{
    // vBattTimer->stop();
    if (m_operation)
    {
        m_operation->requestStop();
    }
}
/*
void FlashEcuSubaruUnisiaJecsM32r::read_batt_voltage()
{
    serial->signal_to_read_vbatt = true;
    //unsigned int vBatt = serial->read_batt_voltage();
    QString vBattText = QString("Battery: %1").arg(serial->vBatt/1000.0, 0, 'f', 3) + " V";
    ui->vBattLabel->setText(vBattText);
    emit LOG_D(vBattText, true, true);
}
*/

void FlashEcuSubaruUnisiaJecsM32r::set_progressbar_value(int value)
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
