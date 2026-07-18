#include "flash_tcu_subaru_denso_sh705x_can.h"

#include <utility>
#include "flash_tcu_subaru_denso_sh705x_can_operation.h"
#include "serial_port_actions.h"

FlashTcuSubaruDensoSH705xCan::FlashTcuSubaruDensoSH705xCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent)
    : QDialog(parent), ecuCalDef(ecuCalDef), cmd_type(cmd_type), ui{std::make_unique<Ui::EcuOperationsWindow>()}
{
    // TCU 0xFFFF3000 0x2000

    ui->setupUi(this);

    if (cmd_type == "test_write")
    {
        this->setWindowTitle("Test write ROM " + ecuCalDef->FileName + " to TCU");
    }
    else if (cmd_type == "write")
    {
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to TCU");
    }
    else if (cmd_type == "read")
    {
        this->setWindowTitle("Read ROM from TCU");
    }

    this->serial = serial;
}

void FlashTcuSubaruDensoSH705xCan::run()
{
    this->show();
    set_progressbar_value(0);

    int tcuAction = 1;

    emit external_logger("Starting");

    // Picking which TCU action to perform needs no serial-> access, so it
    // stays on the GUI thread pre-flight; see class comment in
    // flash_tcu_subaru_denso_sh705x_can_operation.h.
    if (cmd_type == "read")
    {
        QMessageBox msgBox;
        msgBox.setText("Choose which option");
        msgBox.setInformativeText("Perform TCU ROM Dump, Relearn, Read Parmeter or Set Parameter?");
        QPushButton *pButtonDump = msgBox.addButton("Dump", QMessageBox::YesRole);
        QPushButton *pButtonRelearn = msgBox.addButton("Relearn", QMessageBox::YesRole);
        QPushButton *pButtonReadParam = msgBox.addButton("Read Param", QMessageBox::YesRole);
        QPushButton *pButtonSetParam = msgBox.addButton("Set Param", QMessageBox::YesRole);

        msgBox.exec();

        if (msgBox.clickedButton() == (QAbstractButton *)pButtonDump)
        {
            emit LOG_I("Read memory with flashmethod '" + ecuCalDef->FlashMethod + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
            tcuAction = 1;
        }
        else if (msgBox.clickedButton() == (QAbstractButton *)pButtonRelearn)
        {
            emit LOG_I("Attempting TCU relearn", true, true);
            tcuAction = 2;
        }
        else if (msgBox.clickedButton() == (QAbstractButton *)pButtonReadParam)
        {
            emit LOG_I("Attempting to read TCU parameters", true, true);
            tcuAction = 3;
        }
        else if (msgBox.clickedButton() == (QAbstractButton *)pButtonSetParam)
        {
            emit LOG_I("Attempting to set TCU parameters", true, true);
            tcuAction = 4;
        }
        else
        {
            emit LOG_I("No option selected", true, true);
        }
    }

    int ret = QMessageBox::warning(this, tr("Connecting to TCU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to TCU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
    case QMessageBox::Ok:
    {
        m_operation = new FlashTcuSubaruDensoSH705xCanOperation(serial, ecuCalDef, cmd_type, tcuAction, this);
        connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashTcuSubaruDensoSH705xCan::LOG_E);
        connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashTcuSubaruDensoSH705xCan::LOG_W);
        connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashTcuSubaruDensoSH705xCan::LOG_I);
        connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashTcuSubaruDensoSH705xCan::LOG_D);
        connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                this, [this](QString msg)
                { emit external_logger(std::move(msg)); });
        connect(m_operation, &FlashOperationWorker::progressChanged,
                this, &FlashTcuSubaruDensoSH705xCan::set_progressbar_value);

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
            QMessageBox::information(this, tr("TCU Operation"), "TCU operation was succesful, press OK to exit");
            this->close();
        }
        else
        {
            QMessageBox::warning(this, tr("TCU Operation"), "TCU operation failed, press OK to exit and try again");
        }
        break;
    }
    case QMessageBox::Cancel:
        LOG_D("Operation canceled", true, true);
        this->close();
        break;
    default:
        QMessageBox::warning(this, tr("Connecting to ECU"), "Unknown operation selected!");
        LOG_D("Unknown operation selected!", true, true);
        this->close();
        break;
    }
}

FlashTcuSubaruDensoSH705xCan::~FlashTcuSubaruDensoSH705xCan()
{
}

void FlashTcuSubaruDensoSH705xCan::closeEvent(QCloseEvent *event)
{
    if (m_operation)
    {
        m_operation->requestStop();
    }
}

void FlashTcuSubaruDensoSH705xCan::set_progressbar_value(int value)
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
