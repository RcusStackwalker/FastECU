#include "flash_ecu_subaru_unisia_jecs.h"
#include "flash_ecu_subaru_unisia_jecs_operation.h"
#include "modules/flash_utils.h"
#include "serial_port_actions.h"

#include <kernelmemorymodels.h>

#include <utility>

FlashEcuSubaruUnisiaJecs::FlashEcuSubaruUnisiaJecs(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent)
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

void FlashEcuSubaruUnisiaJecs::run()
{
    this->show();
    set_progressbar_value(0);

    // result = init_flash_denso_kline_04(ecuCalDef, cmd_type);

    QString mcu_type_string = ecuCalDef->McuType;
    int mcu_type_index = FlashUtils::findFlashDeviceIndex(mcu_type_string);
    if (mcu_type_index < 0)
    {
        QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, selected MCU does not exists!");
        return;
    }
    QString mcu_name = flashdevices[mcu_type_index].name;
    emit LOG_D("MCU type: " + mcu_name + " " + mcu_type_string + " and index: " + QString::number(mcu_type_index), true, true);

    QString kernel = ecuCalDef->Kernel;
    QString flash_method = ecuCalDef->FlashMethod;

    emit external_logger("Starting");

    if (cmd_type == "read")
    {
        emit LOG_I("Read memory with flashmethod '" + flash_method + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
    }
    else if (cmd_type == "write")
    {
        uint32_t chk_start = 0x1000;
        uint32_t chk_end = flashdevices[mcu_type_index].romsize;
        uint32_t chk_sum = 0;
        uint32_t chk_xor = 0;
        uint32_t chk_sum_rom = ecuCalDef->FullRomData.at(chk_start + 6);
        uint32_t chk_xor_rom = ecuCalDef->FullRomData.at(chk_start + 7);
        for (uint32_t i = chk_start; i < chk_end; i++)
        {
            if (i < (chk_start + 6) || i > (chk_start + 7))
            {
                chk_sum += ecuCalDef->FullRomData.at(i);
                chk_xor ^= ecuCalDef->FullRomData.at(i);
            }
        }
        QByteArray msg;
        emit LOG_I("Checksums:", true, true);
        msg = QString("SUM: 0x%1 | 0x%2").arg((chk_sum & 0xff), 2, 16, QLatin1Char(' ')).arg((chk_sum_rom & 0xff), 2, 16, QLatin1Char(' ')).toUtf8();
        emit LOG_I(msg, true, true);
        msg = QString("XOR: 0x%1 | 0x%2").arg((chk_xor & 0xff), 2, 16, QLatin1Char(' ')).arg((chk_xor_rom & 0xff), 2, 16, QLatin1Char(' ')).toUtf8();
        emit LOG_I(msg, true, true);

        return;
    }

    int ret = QMessageBox::warning(this, tr("Connecting to ECU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to ECU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
    case QMessageBox::Ok:
    {
        m_operation = new FlashEcuSubaruUnisiaJecsOperation(serial, ecuCalDef, cmd_type, this);
        connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuSubaruUnisiaJecs::LOG_E);
        connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuSubaruUnisiaJecs::LOG_W);
        connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuSubaruUnisiaJecs::LOG_I);
        connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuSubaruUnisiaJecs::LOG_D);
        connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                this, [this](QString msg)
                { emit external_logger(std::move(msg)); });
        connect(m_operation, &FlashOperationWorker::progressChanged,
                this, &FlashEcuSubaruUnisiaJecs::set_progressbar_value);

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

FlashEcuSubaruUnisiaJecs::~FlashEcuSubaruUnisiaJecs()
{
}

void FlashEcuSubaruUnisiaJecs::closeEvent(QCloseEvent *bar)
{
    if (m_operation)
    {
        m_operation->requestStop();
    }
}

void FlashEcuSubaruUnisiaJecs::set_progressbar_value(int value)
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
