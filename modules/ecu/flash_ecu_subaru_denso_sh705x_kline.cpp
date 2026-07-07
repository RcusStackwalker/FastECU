#include <modules/ecu/flash_ecu_subaru_denso_sh705x_kline.h>
#include "flash_ecu_subaru_denso_sh705x_kline_operation.h"
#include "serial_port_actions.h"

FlashEcuSubaruDensoSH705xKline::FlashEcuSubaruDensoSH705xKline(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent)
    : QDialog(parent), ecuCalDef(ecuCalDef), cmd_type(cmd_type), ui{std::make_unique<Ui::EcuOperationsWindow>()}
{
    ui->setupUi(this);

    if (cmd_type == "test_write")
        this->setWindowTitle("Test write ROM " + ecuCalDef->FileName + " to ECU");
    else if (cmd_type == "write")
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to ECU");
    else if (cmd_type == "read")
        this->setWindowTitle("Read ROM from ECU");

    this->serial = serial;
}

void FlashEcuSubaruDensoSH705xKline::run()
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
        m_operation = new FlashEcuSubaruDensoSH705xKlineOperation(serial, ecuCalDef, cmd_type, this);
        connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuSubaruDensoSH705xKline::LOG_E);
        connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuSubaruDensoSH705xKline::LOG_W);
        connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuSubaruDensoSH705xKline::LOG_I);
        connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuSubaruDensoSH705xKline::LOG_D);
        connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                this, [this](QString msg)
                { emit external_logger(msg); });
        connect(m_operation, &FlashOperationWorker::progressChanged,
                this, &FlashEcuSubaruDensoSH705xKline::set_progressbar_value);

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

FlashEcuSubaruDensoSH705xKline::~FlashEcuSubaruDensoSH705xKline()
{
}

void FlashEcuSubaruDensoSH705xKline::closeEvent(QCloseEvent *event)
{
    if (m_operation)
        m_operation->requestStop();
}

void FlashEcuSubaruDensoSH705xKline::set_progressbar_value(int value)
{
    bool valueChanged = true;
    if (ui->progressbar)
    {
        valueChanged = ui->progressbar->value() != value;
        ui->progressbar->setValue(value);
    }
    if (valueChanged)
        emit external_logger(value);
}
