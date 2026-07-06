#include "flash_ecu_subaru_denso_mc68hc16y5_02_bdm.h"
#include "flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.h"
#include "serial_port_actions.h"

FlashEcuSubaruDensoMC68HC16Y5_02_BDM::FlashEcuSubaruDensoMC68HC16Y5_02_BDM(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent)
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

void FlashEcuSubaruDensoMC68HC16Y5_02_BDM::run()
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
            m_operation = new FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation(serial, ecuCalDef, cmd_type, this);
            connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuSubaruDensoMC68HC16Y5_02_BDM::LOG_E);
            connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuSubaruDensoMC68HC16Y5_02_BDM::LOG_W);
            connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuSubaruDensoMC68HC16Y5_02_BDM::LOG_I);
            connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuSubaruDensoMC68HC16Y5_02_BDM::LOG_D);
            connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                    this, [this](QString msg) { emit external_logger(msg); });
            connect(m_operation, &FlashOperationWorker::progressChanged,
                    this, &FlashEcuSubaruDensoMC68HC16Y5_02_BDM::set_progressbar_value);

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
            qDebug() << "Operation canceled";
            this->close();
            break;
        default:
            QMessageBox::warning(this, tr("Connecting to ECU"), "Unknown operation selected!");
            qDebug() << "Unknown operation selected!";
            this->close();
            break;
    }
}

FlashEcuSubaruDensoMC68HC16Y5_02_BDM::~FlashEcuSubaruDensoMC68HC16Y5_02_BDM()
{
    delete ui;
}

void FlashEcuSubaruDensoMC68HC16Y5_02_BDM::closeEvent(QCloseEvent *bar)
{
    if (m_operation)
        m_operation->requestStop();
}

void FlashEcuSubaruDensoMC68HC16Y5_02_BDM::set_progressbar_value(int value)
{
    if (ui->progressbar)
        ui->progressbar->setValue(value);
}
