#include "flash_ecu_subaru_denso_mc68hc16y5_02.h"
#include "flash_ecu_subaru_denso_mc68hc16y5_02_operation.h"
#include "serial_port_actions.h"

FlashEcuSubaruDensoMC68HC16Y5_02::FlashEcuSubaruDensoMC68HC16Y5_02(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::EcuOperationsWindow)
    , ecuCalDef(ecuCalDef)
    , cmd_type(cmd_type)
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

FlashEcuSubaruDensoMC68HC16Y5_02::~FlashEcuSubaruDensoMC68HC16Y5_02()
{
    delete ui;
}

void FlashEcuSubaruDensoMC68HC16Y5_02::closeEvent(QCloseEvent *bar)
{
    if (m_operation)
        m_operation->requestStop();
}

void FlashEcuSubaruDensoMC68HC16Y5_02::run()
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
            m_operation = new FlashEcuSubaruDensoMC68HC16Y5_02Operation(serial, ecuCalDef, cmd_type, this);
            connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuSubaruDensoMC68HC16Y5_02::LOG_E);
            connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuSubaruDensoMC68HC16Y5_02::LOG_W);
            connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuSubaruDensoMC68HC16Y5_02::LOG_I);
            connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuSubaruDensoMC68HC16Y5_02::LOG_D);
            connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                    this, [this](QString msg) { emit external_logger(msg); });
            connect(m_operation, &FlashOperationWorker::progressChanged,
                    this, &FlashEcuSubaruDensoMC68HC16Y5_02::set_progressbar_value);

            QEventLoop loop;
            bool success = false;
            connect(m_operation, &FlashOperationWorker::operationFinished, &loop,
                    [&success, &loop](bool ok) { success = ok; loop.quit(); });

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

void FlashEcuSubaruDensoMC68HC16Y5_02::set_progressbar_value(int value)
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
