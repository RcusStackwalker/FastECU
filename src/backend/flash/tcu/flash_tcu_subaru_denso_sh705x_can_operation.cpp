#include "src/backend/flash/tcu/flash_tcu_subaru_denso_sh705x_can_operation.h"
#include "src/backend/flash/flash_utils.h"
#include "src/algorithms/protocol/ssm/ssm_protocol.h"
#include "serial_port_actions.h"

#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <utility>

FlashTcuSubaruDensoSH705xCanOperation::FlashTcuSubaruDensoSH705xCanOperation(
    SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
    QString cmd_type, int tcuAction, QWidget *dialog, QObject *parent, PromptFn promptOverride)
    : FlashOperationWorker(dialog, parent, std::move(promptOverride)), tcuAction(tcuAction), serial(serial), ecuCalDef(ecuCalDef), cmd_type(std::move(cmd_type))
{
}

bool FlashTcuSubaruDensoSH705xCanOperation::execute()
{
    int result = STATUS_ERROR;
    bool ok = false;

    // result = init_flash_denso_subarucan();

    mcu_type_string = ecuCalDef->McuType;
    mcu_type_index = FlashUtils::findFlashDeviceIndex(mcu_type_string);
    if (mcu_type_index < 0)
    {
        emit LOG_E("Unknown MCU type: " + mcu_type_string, true, true);
        return false;
    }
    QString mcu_name = flashdevices[mcu_type_index].name;
    emit LOG_D("MCU type: " + mcu_name + " " + mcu_type_string + " and index: " + QString::number(mcu_type_index), true, true);

    kernel = ecuCalDef->Kernel;
    flash_method = ecuCalDef->FlashMethod;

    if (cmd_type == "test_write")
    {
        test_write = true;
        emit LOG_I("Test write memory with flashmethod '" + flash_method + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
    }
    else if (cmd_type == "write")
    {
        test_write = false;
        emit LOG_I("Write memory with flashmethod '" + flash_method + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
    }

    // Set serial port
    FlashUtils::configureIso15765Can(serial, "500000", 0x7E1, 0x7E9);
    // Open serial port
    serial->open_serial_port();

    if (tcuAction == 1)
    {
        emit LOG_I("Connecting to Subaru Denso CAN bootloader, please wait...", true, true);
        result = connect_bootloader();

        if (result == STATUS_SUCCESS && !kernel_alive)
        {
            emit externalLoggerMessage("Preparing, please wait...");
            emit LOG_I("Initializing Subaru Denso CAN kernel upload, please wait...", true, true);
            result = upload_kernel(kernel, ecuCalDef->KernelStartAddr.toUInt(&ok, 16));
        }
        if (result == STATUS_SUCCESS)
        {
            if (cmd_type == "read")
            {
                emit externalLoggerMessage("Reading ROM, please wait...");
                emit LOG_I("Reading ROM from Subaru Denso using CAN", true, true);
                result = read_mem(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
            }
            else if (cmd_type == "test_write" || cmd_type == "write")
            {
                emit externalLoggerMessage("Writing ROM, please wait...");
                emit LOG_I("Writing ROM to Subaru Denso using CAN (beta status)", true, true);
                result = write_mem(test_write);
            }
        }
    }
    else if (tcuAction == 2)
    {
        emit LOG_I("Commencing TCU relearn process, please wait...", true, true);
        result = tcu_relearn_subaru_ssm();
    }
    else if (tcuAction == 3)
    {
        emit LOG_I("Commencing to read TCU parameters, please wait...", true, true);
        result = tcu_readparam_subaru_ssm();
    }
    else if (tcuAction == 4)
    {
        emit LOG_I("Commencing to set TCU parameters, please wait...", true, true);
        result = tcu_setparam_subaru_ssm();
    }

    return result == STATUS_SUCCESS;
}

/*
 * Runs QInputDialog::getInt on the GUI thread and blocks this (worker)
 * thread until the user answers. Mirrors FlashOperationWorker::confirm().
 */
int FlashTcuSubaruDensoSH705xCanOperation::promptInt(const QString& title, const QString& label, int value,
                                                     int minValue, int maxValue, int step, bool *ok)
{
    int result = value;
    bool okResult = false;
    QWidget *dialog = m_dialog;
    QMetaObject::invokeMethod(dialog, [dialog, title, label, value, minValue, maxValue, step, &okResult]()
                              { return QInputDialog::getInt(dialog, title, label, value, minValue, maxValue, step, &okResult); }, Qt::BlockingQueuedConnection, &result);
    if (ok)
    {
        *ok = okResult;
    }
    return result;
}

/*
 * Set SSM params for Denso CAN (iso15765) 32bit ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::tcu_setparam_subaru_ssm()
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;

    // CAN 0xb8 command is disabled, so switch to K-Line comms

    emit LOG_I("Changing to K-Line comms...", true, true);
    serial->reset_connection();
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(false);
    serial->set_is_iso14230_connection(true);
    serial->open_serial_port();
    serial->change_port_speed("4800");
    serial->set_add_iso14230_header(false);
    tester_id = 0xF0;
    target_id = 0x18;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Change to K-Line comms successful...", true, true);

    bool ok;
    int correction_1to2 = promptInt("Enter SSM Parameter Data", "1->2 Pressure Correction (DC):", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int correction_2to3 = promptInt("Enter SSM Parameter Data", "2->3 Pressure Correction (HLRC):", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int correction_3to4 = promptInt("Enter SSM Parameter Data", "3->4 Pressure Correction (IC):", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int correction_4to5 = promptInt("Enter SSM Parameter Data", "4->5 Pressure Correction (FB):", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int correction_fwdb = promptInt("Enter SSM Parameter Data", "Fwd Brake Pressure Correction:", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int correction_4wd = promptInt("Enter SSM Parameter Data", "4WD Pressure Correction:", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int correction_pl = promptInt("Enter SSM Parameter Data", "Line Pressure Correction:", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int temp_basis = promptInt("Enter SSM Parameter Data", "Temp Basis for Corrections:", 0, 0, 255, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }
    int torque_correction_awd = promptInt("Enter SSM Parameter Data", "AWD Torque Correction:", 0, 0, 65535, 1, &ok);
    if (!ok)
    {
        return STATUS_ERROR;
    }

    emit LOG_I("Setting TCU parameters...", true, true);

    output.append((uint8_t)0xB8);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x6c); // 0x16c - IC Correction, 3->4
    output.append((uint8_t)(correction_3to4 & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0x6d); // 0x16d - HLRC Correction, 2->3
    output[4] = ((uint8_t)(correction_2to3 & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0x6e); // 0x16e - DC Correction, 1->2
    output[4] = ((uint8_t)(correction_1to2 & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0x6f); // 0x16f - FB correction
    output[4] = ((uint8_t)(correction_4to5 & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0x70); // 0x170 - AWD Clutch Torque High
    output[4] = ((uint8_t)((torque_correction_awd >> 8) & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0x71); // 0x171 - AWD Clutch Torque Low
    output[4] = ((uint8_t)(torque_correction_awd & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0xbc); // 0x1bc - FwdB correction
    output[4] = ((uint8_t)(correction_fwdb & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0xbd); // 0x1bd - 4WD correction
    output[4] = ((uint8_t)(correction_4wd & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0xbe); // 0x1be - PL correction
    output[4] = ((uint8_t)(correction_pl & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[3] = ((uint8_t)0xbf); // 0x1bf - Temp basis
    output[4] = ((uint8_t)(temp_basis & 0xff));
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_I("Saving to TCU...", true, true);
    output[2] = ((uint8_t)0x00);
    output[3] = ((uint8_t)0xec); // 0xec
    output[4] = ((uint8_t)0x55);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output[4] = ((uint8_t)0xaa);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);
    received = serial->read_serial_data(receive_timeout);
    emit LOG_D("Response:" + SsmProtocol::toHex(received), true, true);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_I("Changing back to CAN comms...", true, true);
    // Set serial port
    serial->reset_connection();
    FlashUtils::configureIso15765Can(serial, "500000", 0x7E1, 0x7E9);
    // Open serial port
    serial->open_serial_port();

    return STATUS_SUCCESS;
}

/*
 * Read SSM params from Subaru Denso CAN (iso15765) 32bit ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::tcu_readparam_subaru_ssm()
{
    QByteArray output;
    QByteArray received;
    bool responseOK;
    int try_count;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    serial->set_add_iso14230_header(false);

    emit LOG_I("Reading TCU parameters...", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0xA8);
    output.append((uint8_t)0x00); // one time only
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x6c); // 0x16c
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x6d); // 0x16d
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x6e); // 0x16e
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x6f); // 0x16f
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x70); // 0x170
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x71); // 0x171
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0xbc); // 0x1bc
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0xbd); // 0x1bd
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0xbe); // 0x1be
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0xbf); // 0x1bf

    try_count = 0;
    responseOK = false;
    while (try_count < 6 && !responseOK)
    {
        serial->write_serial_data_echo_check(output);

        received = serial->read_serial_data(serial_read_short_timeout);
        if (received.length() > 4)
        {
            if (((uint8_t)received[4] == 0xF8))
            {
                responseOK = true;
            }
        }
        try_count++;
    }

    if (!responseOK)
    {
        return STATUS_ERROR;
    }
    if (received.length() > 10)
    {
        if ((uint8_t)received.at(4) != 0xE8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    uint16_t torque_correction_awd = ((uint8_t)received[9] << 8) + (uint8_t)received[10];

    emit LOG_I("Input Clutch Pressure Correction (raw byte): " + QString::number((uint8_t)received[5]), true, true);
    emit LOG_I("High Low Reverse Clutch Pressure Correction (raw byte): " + QString::number((uint8_t)received[6]), true, true);
    emit LOG_I("Direct Clutch Pressure Correction (raw byte): " + QString::number((uint8_t)received[7]), true, true);
    emit LOG_I("Front Brake Pressure Correction (raw byte): " + QString::number((uint8_t)received[8]), true, true);
    emit LOG_I("Correction of AWD Clutch Torque (raw word): " + QString::number((uint16_t)torque_correction_awd), true, true);
    emit LOG_I("Forward Brake Pressure Correction (raw byte): " + QString::number((uint8_t)received[11]), true, true);
    emit LOG_I("4WD Pressure Correction (raw byte): " + QString::number((uint8_t)received[12]), true, true);
    emit LOG_I("Line Pressure Correction (raw byte): " + QString::number((uint8_t)received[13]), true, true);
    emit LOG_I("Temperature basis for above Pressure Corrections (raw byte): " + QString::number((uint8_t)received[14]), true, true);

    return STATUS_SUCCESS;
}
/*
 * TCU relearn for Subaru Denso CAN (iso15765) bootloader 32bit ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::tcu_relearn_subaru_ssm()
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;
    bool responseOK;
    int try_count;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    serial->set_add_iso14230_header(false);

    confirm(tr("TCU Relearn"), tr("Engine must be at operating temperature. Car must be off the ground! Start with Engine off, Ignition on, stick in P, press OK to continue"),
            QMessageBox::Ok, QMessageBox::Ok);

    emit LOG_I("Initialising TCU relearn, step 1...", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0xB8);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0xFC);
    output.append((uint8_t)0x01);
    try_count = 0;
    responseOK = false;
    while (try_count < 6 && !responseOK)
    {
        serial->write_serial_data_echo_check(output);

        received = serial->read_serial_data(serial_read_short_timeout);
        if (received.length() > 4)
        {
            if (((uint8_t)received[4] == 0xF8))
            {
                responseOK = true;
            }
        }
        try_count++;
    }
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            // return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        // return STATUS_ERROR;
    }

    emit LOG_I("Initialising TCU relearn, step 2......", true, true);
    output[7] = ((uint8_t)0xFD);
    output[8] = ((uint8_t)0x09);
    try_count = 0;
    responseOK = false;
    while (try_count < 6 && !responseOK)
    {
        serial->write_serial_data_echo_check(output);

        received = serial->read_serial_data(serial_read_short_timeout);
        if (received.length() > 4)
        {
            if (((uint8_t)received[4] == 0xF8))
            {
                responseOK = true;
            }
        }
        try_count++;
    }
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            // return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        // return STATUS_ERROR;
    }

    confirm(tr("TCU Relearn"), tr("Start Engine, let revs settle, move stick into D, fully press brake, press OK to continue"),
            QMessageBox::Ok, QMessageBox::Ok);

    emit LOG_I("Tracking relearn status......", true, true);
    output[4] = ((uint8_t)0xA8);
    output[5] = ((uint8_t)0x00);
    output[6] = ((uint8_t)0x00);
    output[7] = ((uint8_t)0x01);
    output[8] = ((uint8_t)0xFC);
    output[9] = ((uint8_t)0x00);
    output[10] = ((uint8_t)0x01);
    output[11] = ((uint8_t)0xFD);
    try_count = 0;
    responseOK = false;
    while (try_count < 200 && !responseOK)
    {
        serial->write_serial_data_echo_check(output);

        received = serial->read_serial_data(serial_read_short_timeout);
        if (received.length() > 4)
        {
            if (((uint8_t)received[4] == 0xF8))
            {
                responseOK = true;
            }
        }
        try_count++;
    }
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xF8)
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            // return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        // return STATUS_ERROR;
    }

    return STATUS_ERROR;
}

/*
 * Connect to Subaru Denso CAN (iso15765) bootloader 32bit ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::connect_bootloader()
{
    QByteArray output;
    QByteArray received;
    QByteArray seed;
    QByteArray seed_key;
    QByteArray msg;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    emit LOG_I("Checking if kernel is already running...", true, true);
    emit LOG_I("Requesting kernel ID", true, true);
    received.clear();
    received = request_kernel_id();
    if (received.length() > 8)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_ID | 0x40))
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);
        }
        else
        {
            received.remove(0, 9);
            emit LOG_I("Kernel ID: " + received, true, true);

            kernel_alive = true;
            return STATUS_SUCCESS;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);
    }

    emit LOG_I("No response from kernel, initialising ECU...", true, true);

    emit LOG_I("Requesting ECU ID", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0xAA);
    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) == 0xEA)
        {
            QByteArray response = received;
            response.remove(0, 8);
            response.remove(5, response.length() - 5);

            QString ecuid;
            for (int i = 0; i < 5; i++)
            {
                ecuid.append(QString("%1").arg((uint8_t)response.at(i), 2, 16, QLatin1Char('0')).toUpper());
            }
            emit LOG_I("ECU ID: " + ecuid, true, true);
            if (cmd_type == "read")
            {
                ecuCalDef->RomId = ecuid + "_";
            }
        }
        else
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);
    }

    emit LOG_I("Requesting CAL ID", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x09);
    output.append((uint8_t)0x04);
    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) == 0x49 && (uint8_t)received.at(5) == 0x04)
        {
            QByteArray response = received;
            response.remove(0, 7);

            emit LOG_I("CAL ID: " + response, true, true);
            if (cmd_type == "read")
            {
                ecuCalDef->RomId.insert(0, QString(response) + "_");
            }
        }
        else
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);
    }

    emit LOG_I("Requesting session mode", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x10);
    output.append((uint8_t)0x03);
    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != 0x50 && (uint8_t)received.at(5) != 0x03)
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x27);
    output.append((uint8_t)0x01);

    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != 0x67 || (uint8_t)received.at(5) != 0x01)
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_I("Seed request ok", true, true);

    seed.clear();
    seed.append(received.at(6));
    seed.append(received.at(7));
    seed.append(received.at(8));
    seed.append(received.at(9));

    seed_key = generate_seed_key(seed);

    emit LOG_I("Sending seed key", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x27);
    output.append((uint8_t)0x02);
    output.append(seed_key);

    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != 0x67 || (uint8_t)received.at(5) != 0x02)
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_I("Seed key ok", true, true);

    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x10);
    output.append((uint8_t)0x02);
    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) == 0x50 && ((uint8_t)received.at(5) == 0x02 || (uint8_t)received.at(5) == 0x42))
        {
            QByteArray response = received;
            response.remove(0, 6);
            QString msg;
            msg.clear();
            for (int i = 0; i < response.length(); i++)
            {
                msg.append(QString("%1").arg((uint8_t)response.at(i), 2, 16, QLatin1Char('0')).toUpper());
            }
        }
        else
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }
    emit LOG_I("Succesfully set to programming session", true, true);

    return STATUS_SUCCESS;
}

/*
 * Upload kernel to Subaru Denso CAN (iso15765) 32bit ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::upload_kernel(const QString& kernel, uint32_t kernel_start_addr)
{
    QFile file(kernel);

    QByteArray output;
    QByteArray received;
    QByteArray pl_encr;
    uint32_t file_len = 0;
    uint32_t pl_len = 0;
    uint32_t start_address = 0;
    uint32_t end_addr = 0;
    uint32_t chk_sum = 0;
    uint32_t blockaddr = 0;
    uint16_t blockno = 0;
    uint16_t maxblocks = 0;

    start_address = kernel_start_addr;
    emit LOG_D("Start address to upload kernel: 0x" + QString::number(start_address, 16), true, true);

    if (!serial->is_serial_port_open())
    {
        emit LOG_E("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    // Check kernel file
    if (!file.open(QIODevice::ReadOnly))
    {
        emit LOG_E("Unable to open kernel file for reading", true, true);
        return STATUS_ERROR;
    }
    file_len = file.size();
    pl_len = (file_len + 3) & ~3;
    pl_encr = file.readAll();
    maxblocks = pl_len / 128;
    if ((pl_len % 128) != 0)
    {
        maxblocks++;
    }
    end_addr = (start_address + (maxblocks * 128)) & 0xFFFFFFFF;
    uint32_t data_len = end_addr - start_address;
    while ((uint32_t)pl_encr.length() < data_len)
    {
        pl_encr.append((uint8_t)0x00);
    }
    pl_encr.remove(pl_encr.length() - 4, 4);
    chk_sum = 0;
    for (int i = 0; i < pl_encr.length(); i += 4)
    {
        chk_sum += ((pl_encr.at(i) << 24) & 0xFF000000) | ((pl_encr.at(i + 1) << 16) & 0xFF0000) | ((pl_encr.at(i + 2) << 8) & 0xFF00) | ((pl_encr.at(i + 3)) & 0xFF);
    }
    chk_sum = 0x5aa5a55a - chk_sum;

    pl_encr.append((uint8_t)((chk_sum >> 24) & 0xFF));
    pl_encr.append((uint8_t)((chk_sum >> 16) & 0xFF));
    pl_encr.append((uint8_t)((chk_sum >> 8) & 0xFF));
    pl_encr.append((uint8_t)(chk_sum & 0xFF));
    pl_encr = encrypt_payload(pl_encr, pl_encr.length());
    // pl_encr = decrypt_payload(pl_encr, pl_encr.length());

    emit progressChanged(0);

    emit LOG_I("Initialize kernel upload", true, true);
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x34);
    output.append((uint8_t)0x04);
    output.append((uint8_t)0x33);
    output.append((uint8_t)((start_address >> 16) & 0xFF));
    output.append((uint8_t)((start_address >> 8) & 0xFF));
    output.append((uint8_t)(start_address & 0xFF));
    output.append((uint8_t)((data_len >> 16) & 0xFF));
    output.append((uint8_t)((data_len >> 8) & 0xFF));
    output.append((uint8_t)(data_len & 0xFF));

    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) == 0x74 && (uint8_t)received.at(5) == 0x20)
        {
        }
        else
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_I("Uploading kernel, please wait...", true, true);
    for (blockno = 0; blockno <= maxblocks; blockno++)
    {
        if (stopRequested())
        {
            return 0;
        }

        blockaddr = start_address + blockno * 128;
        output.clear();
        output.append((uint8_t)0x00);
        output.append((uint8_t)0x00);
        output.append((uint8_t)0x07);
        output.append((uint8_t)0xE1);
        output.append((uint8_t)0xB6);
        output.append((uint8_t)(blockaddr >> 16) & 0xFF);
        output.append((uint8_t)(blockaddr >> 8) & 0xFF);
        output.append((uint8_t)blockaddr & 0xFF);

        if (blockno == maxblocks)
        {
            for (uint32_t i = 0; i < data_len; i++)
            {
                output.append(pl_encr.at(i + blockno * 128));
            }
        }
        else
        {
            for (int i = 0; i < 128; i++)
            {
                output.append(pl_encr.at(i + blockno * 128));
            }
            data_len -= 128;
        }

        serial->write_serial_data_echo_check(output);
        received = serial->read_serial_data(serial_read_timeout);

        float pleft = (float)blockno / (float)maxblocks * 100;
        emit progressChanged(pleft);
    }

    emit LOG_I("Kernel uploaded, starting...", true, true);

    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x37);

    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0x77)
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)0x31);
    output.append((uint8_t)0x01);
    output.append((uint8_t)0x02);
    output.append((uint8_t)0x02);
    output.append((uint8_t)0x02);

    serial->write_serial_data_echo_check(output);

    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0x71)
        {
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit progressChanged(100);

    emit LOG_I("Kernel started, initializing...", true, true);

    // switch to simple CAN comms for kernel (kernel has no iso15765 support)
    serial->reset_connection();
    serial->set_is_iso14230_connection(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(true);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("500000");
    serial->set_iso15765_source_address(0x7e1);
    serial->set_iso15765_destination_address(0x7e9);
    // Open serial port
    serial->open_serial_port();

    delay(500);

    emit LOG_I("Requesting kernel ID", true, true);
    received.clear();
    received = request_kernel_id();
    if (received.length() > 8)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_ID | 0x40))
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);
        }
        else
        {
            received.remove(0, 9);
            emit LOG_I("Kernel ID: " + received, true, true);

            kernel_alive = true;
            return STATUS_SUCCESS;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);
    }

    return STATUS_SUCCESS;
}

/*
 * Read memory from Subaru Denso CAN (iso15765) 32bit TCUs, nisprog kernel
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::read_mem(uint32_t start_addr, uint32_t length)
{
    QElapsedTimer timer;
    QByteArray output;
    QByteArray received;
    QByteArray msg;
    QByteArray pagedata;
    QByteArray mapdata;
    uint32_t cplen = 0;
    uint32_t timeout = 0;

    uint32_t datalen = 6;
    uint32_t pagesize = 0x0400;

    uint32_t skip_start = start_addr & (pagesize - 1); // if unaligned, we'll be receiving this many extra bytes
    uint32_t addr = start_addr - skip_start;
    uint32_t willget = (skip_start + length + pagesize - 1) & ~(pagesize - 1);
    uint32_t len_done = 0; // total data written to file

    emit LOG_I("Start reading ROM, please wait..." + received, true, true);

    // send 0xD8 command to kernel to dump the chunk from ROM
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)SUB_KERNEL_READ_AREA);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);

    emit progressChanged(0);

    mapdata.clear();

    while (willget)
    {
        if (stopRequested())
        {
            return STATUS_ERROR;
        }

        uint32_t numblocks = 1;
        unsigned curspeed = 0, tleft;
        float pleft = 0;
        unsigned long chrono;

        // uint32_t curblock = (addr / pagesize);

        pleft = (float)(addr - start_addr) / (float)length * 100.0f;
        emit progressChanged(pleft);

        output[9] = (uint8_t)0x00 & 0xFF;
        output[10] = (uint8_t)(addr >> 16) & 0xFF;
        output[11] = (uint8_t)(addr >> 8) & 0xFF;
        output[12] = (uint8_t)addr & 0xFF;
        output[13] = (uint8_t)(pagesize >> 8) & 0xFF;
        output[14] = (uint8_t)pagesize & 0xFF;
        serial->write_serial_data_echo_check(output);
        //
        // delay(10);
        received = serial->read_serial_data(serial_read_timeout);
        //

        if (received.length() > 8)
        {
            if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_READ_AREA | 0x40))
            {
                emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

                return STATUS_ERROR;
            }
        }
        else
        {
            emit LOG_E("No valid response from ECU", true, true);

            return STATUS_ERROR;
        }

        received.remove(0, 9);
        mapdata.append(received);

        // don't count skipped first bytes //
        cplen = (numblocks * pagesize) - skip_start; // this is the actual # of valid bytes in buf[]
        skip_start = 0;

        chrono = timer.elapsed();
        timer.start();

        if (cplen > 0 && chrono > 0)
        {
            curspeed = cplen * (1000.0f / chrono);
        }

        if (!curspeed)
        {
            curspeed += 1;
        }

        tleft = (willget / curspeed) % 9999;
        tleft++;

        QString start_address = QString("%1").arg(addr, 8, 16, QLatin1Char('0')).toUpper();
        QString block_len = QString("%1").arg(pagesize, 8, 16, QLatin1Char('0')).toUpper();
        msg = QString("Kernel read addr: 0x%1 length: 0x%2, %3 B/s %4 s").arg(start_address).arg(block_len).arg(curspeed, 6, 10, QLatin1Char(' ')).arg(tleft, 6, 10, QLatin1Char(' ')).toUtf8();
        emit LOG_I(msg, true, true);

        // and drop extra bytes at the end //
        uint32_t extrabytes = (cplen + len_done); // hypothetical new length
        if (extrabytes > length)
        {
            cplen -= (extrabytes - length);
            // thus, (len_done + cplen) will not exceed len
        }

        // increment addr, len, etc //
        len_done += cplen;
        addr += (numblocks * pagesize);
        willget -= (numblocks * pagesize);
    }

    emit LOG_I("ROM read ready", true, true);

    ecuCalDef->FullRomData = mapdata;
    emit progressChanged(100);

    return STATUS_SUCCESS;
}

/*
 * Write memory to Subaru Denso CAN (iso15765) 32bit ECUs, nisprog kernel
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::write_mem(bool test_write)
{
    QByteArray filedata;

    filedata = ecuCalDef->FullRomData;

    QScopedArrayPointer<uint8_t> data_array(new uint8_t[filedata.length()]);

    int block_modified[16] = {0};

    unsigned bcnt = 0;
    unsigned blockno;

    emit progressChanged(0);

    for (int i = 0; i < filedata.length(); i++)
    {
        data_array[i] = filedata.at(i);
    }

    emit LOG_I("--- Comparing ECU flash memory pages to image file ---", true, true);
    emit LOG_I("blk\tstart\tlen\tecu crc\timg crc\tsame?", true, true);

    if (get_changed_blocks(&data_array[0], block_modified))
    {
        emit LOG_E("Error in ROM compare", true, true);
        return STATUS_ERROR;
    }

    bcnt = 0;
    emit LOG_I("Different blocks : ", true, false);
    for (blockno = 0; blockno < flashdevices[mcu_type_index].numblocks; blockno++)
    {
        if (block_modified[blockno])
        {
            emit LOG_I(QString::number(blockno) + ", ", false, false);
            bcnt += 1;
        }
    }
    emit LOG_I(" (total: " + QString::number(bcnt) + ")", false, true);

    if (bcnt)
    {
        flashbytescount = 0;
        flashbytesindex = 0;

        for (blockno = 0; blockno < flashdevices[mcu_type_index].numblocks; blockno++)
        {
            if (block_modified[blockno])
            {
                flashbytescount += flashdevices[mcu_type_index].fblocks[blockno].len;
            }
        }

        emit LOG_I("--- Start writing ROM file to ECU flash memory ---", true, true);
        for (blockno = 0; blockno < flashdevices[mcu_type_index].numblocks; blockno++)
        {
            if (block_modified[blockno])
            {
                if (reflash_block(&data_array[flashdevices[mcu_type_index].fblocks->start], &flashdevices[mcu_type_index], blockno, test_write))
                {
                    emit LOG_I("Block " + QString::number(blockno) + " reflash failed.", true, true);
                    return STATUS_ERROR;
                }
                else
                {
                    flashbytesindex += flashdevices[mcu_type_index].fblocks[blockno].len;
                    emit LOG_I("Block " + QString::number(blockno) + " reflash complete.", true, true);
                }
            }
        }
        emit progressChanged(100);

        emit LOG_I("--- Comparing ECU flash memory pages to image file after reflash ---", true, true);
        emit LOG_I("blk\tstart\tlen\tecu crc\timg crc\tsame?", true, true);

        if (get_changed_blocks(&data_array[0], block_modified))
        {
            emit LOG_E("Error in ROM compare", true, true);
            return STATUS_ERROR;
        }

        bcnt = 0;
        emit LOG_I("Different blocks : ", true, false);
        for (blockno = 0; blockno < flashdevices[mcu_type_index].numblocks; blockno++)
        {
            if (block_modified[blockno])
            {
                emit LOG_I(QString::number(blockno) + ", ", false, false);
                bcnt += 1;
            }
        }
        emit LOG_I(" (total: " + QString::number(bcnt) + ")", false, true);
        if (!test_write)
        {
            if (bcnt)
            {
                emit LOG_E("*** ERROR IN FLASH PROCESS ***", true, true);
                emit LOG_E("Don't power off your ECU, kernel is still running and you can try flashing again!", true, true);
            }
        }
        else
        {
            emit LOG_I("*** Test write PASS, it's ok to perform actual write! ***", true, true);
        }
    }
    else
    {
        emit LOG_I("*** Compare results no difference between ROM and ECU data, no flashing needed! ***", true, true);
    }

    return STATUS_SUCCESS;
}

/*
 * Compare ROM 32bit (iso15765) CAN ECUs
 *
 * @return
 */
int FlashTcuSubaruDensoSH705xCanOperation::get_changed_blocks(const uint8_t *src, int *modified)
{
    unsigned blockno;
    QByteArray msg;

    for (blockno = 0; blockno < flashdevices[mcu_type_index].numblocks; blockno++)
    {
        if (stopRequested())
        {
            return STATUS_ERROR;
        }

        uint32_t bs, blen;
        bs = flashdevices[mcu_type_index].fblocks[blockno].start;
        blen = flashdevices[mcu_type_index].fblocks[blockno].len;

        QString block_no = QString("%1").arg((uint8_t)blockno, 2, 10, QLatin1Char('0')).toUpper();
        QString block_start = QString("%1").arg((uint32_t)bs, 8, 16, QLatin1Char('0')).toUpper();
        QString block_length = QString("%1").arg((uint32_t)blen, 8, 16, QLatin1Char('0')).toUpper();

        msg = QString("FB" + block_no + "\t0x" + block_start + "\t0x" + block_length).toUtf8();
        emit LOG_I(msg, true, false);
        // do CRC comparison with ECU //
        if (check_romcrc(&src[bs], bs, blen, &modified[blockno]))
        {
            return -1;
        }
        delay(5);
    }
    return 0;
}

/*
 * ROM CRC 32bit CAN (iso15765) ECUs
 *
 * @return
 */
int FlashTcuSubaruDensoSH705xCanOperation::check_romcrc(const uint8_t *src, uint32_t addr, uint32_t len, int *modified)
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;
    uint32_t imgcrc32 = 0;
    uint32_t ecucrc32 = 0;
    uint32_t pagesize = len; // Test 32-bit CRC with block size
    uint32_t datalen = 8;

    // Test 32-bit CRC with block size
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)SUB_KERNEL_CRC);
    output.append((uint8_t)(addr >> 24) & 0xFF);
    output.append((uint8_t)(addr >> 16) & 0xFF);
    output.append((uint8_t)(addr >> 8) & 0xFF);
    output.append((uint8_t)addr & 0xFF);
    output.append((uint8_t)0x00 & 0xFF);
    output.append((uint8_t)(pagesize >> 16) & 0xFF);
    output.append((uint8_t)(pagesize >> 8) & 0xFF);
    output.append((uint8_t)pagesize & 0xFF);

    serial->write_serial_data_echo_check(output);
    received.clear();
    received = serial->read_serial_data(serial_read_extra_long_timeout);

    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_CRC | 0x40))
        {
            emit LOG_E("", false, true);
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("", false, true);
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    ecucrc32 = ((uint8_t)received.at(9) << 24) | ((uint8_t)received.at(10) << 16) | ((uint8_t)received.at(11) << 8) | (uint8_t)received.at(12);

    imgcrc32 = SsmProtocol::crc32(src, pagesize);
    msg.clear();
    msg.append(QString("ROM CRC: 0x%1 IMG CRC: 0x%2").arg(ecucrc32, 8, 16, QLatin1Char('0')).arg(imgcrc32, 8, 16, QLatin1Char('0')).toUtf8());
    emit LOG_D(msg, true, true);

    QString ecu_crc32 = QString("%1").arg((uint32_t)ecucrc32, 8, 16, QLatin1Char('0')).toUpper();
    QString img_crc32 = QString("%1").arg((uint32_t)imgcrc32, 8, 16, QLatin1Char('0')).toUpper();
    msg = QString("\t" + ecu_crc32 + "\t" + img_crc32).toUtf8();
    emit LOG_I(msg, false, false);
    if (ecucrc32 != imgcrc32)
    {
        emit LOG_I("\tNO", false, true);
        *modified = 1;
        serial->read_serial_data(serial_read_short_timeout);
        return 0;
    }

    emit LOG_I("\tYES", false, true);
    *modified = 0;
    serial->read_serial_data(serial_read_short_timeout);
    return 0;
}

int FlashTcuSubaruDensoSH705xCanOperation::init_flash_write()
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;

    uint32_t datalen = 0;
    uint16_t chksum;

    emit LOG_I("Check max message length", true, false);
    datalen = 0;
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)(SUB_KERNEL_GET_MAX_MSG_SIZE & 0xFF));
    received = serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 9)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_GET_MAX_MSG_SIZE | 0x40))
        {
            emit LOG_E("", false, true);
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("", false, true);
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    flashmessagesize = (uint8_t)received.at(9) << 24 | (uint8_t)received.at(10) << 16 | (uint8_t)received.at(11) << 8 | (uint8_t)received.at(12) << 0;
    msg.clear();
    msg.append(QString(": 0x%1").arg(flashmessagesize, 4, 16, QLatin1Char('0')).toUtf8());
    emit LOG_I(msg, false, true);

    emit LOG_I("Check flashblock size", true, false);
    datalen = 0;
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)(SUB_KERNEL_GET_MAX_BLK_SIZE & 0xFF));
    received = serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 9)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_GET_MAX_BLK_SIZE | 0x40))
        {
            emit LOG_E("", false, true);
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("", false, true);
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    flashblocksize = (uint8_t)received.at(9) << 24 | (uint8_t)received.at(10) << 16 | (uint8_t)received.at(11) << 8 | (uint8_t)received.at(12) << 0;
    msg.clear();
    msg.append(QString(": 0x%1").arg(flashblocksize, 4, 16, QLatin1Char('0')).toUtf8());
    emit LOG_I(msg, false, true);

    uint8_t SUB_KERNEL_CMD = 0;
    if (test_write)
    {
        SUB_KERNEL_CMD = (uint8_t)(SUB_KERNEL_FLASH_DISABLE & 0xFF);
        emit LOG_I("Test write mode on, no actual flash write is performed", true, true);
    }
    else if (!test_write)
    {
        SUB_KERNEL_CMD = (uint8_t)(SUB_KERNEL_FLASH_ENABLE & 0xFF);
        emit LOG_I("Test write mode off, perform actual flash write", true, true);
    }

    datalen = 0;
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)(SUB_KERNEL_CMD & 0xFF));
    received = serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_CMD | 0x40))
        {
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_E("Flash mode succesfully set", true, true);

    flash_write_init = true;

    return STATUS_SUCCESS;
}

/*
 * Reflash ROM 32bit CAN (iso15765) ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write)
{
    uint32_t block_start;
    uint32_t block_len;
    uint32_t datalen = 0;

    QByteArray output;
    QByteArray received;
    QByteArray msg;

    if (!flash_write_init)
    {
        if (init_flash_write())
        {
            return STATUS_ERROR;
        }
    }

    if (blockno >= fdt->numblocks)
    {
        emit LOG_E("Block " + QString::number(blockno) + " out of range!", true, true);
        return -1;
    }

    block_start = fdt->fblocks[blockno].start;
    block_len = fdt->fblocks[blockno].len;

    QString start_addr = QString("%1").arg((uint32_t)block_start, 8, 16, QLatin1Char('0')).toUpper();
    QString length = QString("%1").arg((uint32_t)block_len, 8, 16, QLatin1Char('0')).toUpper();
    msg = QString("Flash block addr: 0x" + start_addr + " len: 0x" + length).toUtf8();
    emit LOG_I(msg, true, true);

    emit LOG_I("Check flash voltage", true, false);
    datalen = 0;
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)(SUB_KERNEL_PROG_VOLT & 0xFF));
    received = serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_medium_timeout);

    if (received.length() > 7)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_PROG_VOLT | 0x40))
        {
            emit LOG_E("", false, true);
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("", false, true);
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    float prog_voltage = (((uint8_t)received.at(9) << 8) + (uint8_t)received.at(10)) / 50.0;
    emit LOG_I(": " + QString::number(prog_voltage) + "V", false, true);

    if (flash_block(newdata, block_start, block_len))
    {
        emit LOG_E("Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and receiving commands!", true, true);
        return STATUS_ERROR;
    }

    emit LOG_I("Flash block ok", true, true);

    return STATUS_SUCCESS;
}

/*
 * Flash block 32bit CAN (iso15765) ECUs
 *
 * @return success
 */
int FlashTcuSubaruDensoSH705xCanOperation::flash_block(const uint8_t *src, uint32_t start, uint32_t len)
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;

    uint32_t remain = len;
    uint32_t block_start = start;
    uint32_t block_len = len;
    uint32_t byteindex = flashbytesindex;
    uint32_t datalen = 0;
    uint32_t imgcrc32 = 0;
    uint32_t flashblockstart = start;
    uint16_t blocksize = 0x200;

    QElapsedTimer timer;

    unsigned long chrono;
    unsigned curspeed, tleft;

    flashblocksize = 0x1000;

    msg = QString("Flash page erase addr: 0x%1 len: 0x%2").arg(block_start, 8, 16, QLatin1Char('0')).arg(block_len, 8, 16, QLatin1Char('0')).toUtf8();
    emit LOG_I(msg, true, true);

    emit LOG_I("Erasing flash page...", true, false);
    datalen = 4;
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
    output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
    output.append((uint8_t)(datalen + 1) & 0xFF);
    output.append((uint8_t)(SUB_KERNEL_BLANK_PAGE & 0xFF));
    output.append((uint8_t)(start >> 24) & 0xFF);
    output.append((uint8_t)(start >> 16) & 0xFF);
    output.append((uint8_t)(start >> 8) & 0xFF);
    output.append((uint8_t)start & 0xFF);
    received = serial->write_serial_data_echo_check(output);

    // delay(500);
    received = serial->read_serial_data(serial_read_extra_long_timeout);

    if (received.length() > 8)
    {
        if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_BLANK_PAGE | 0x40))
        {
            emit LOG_E("", false, true);
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

            return STATUS_ERROR;
        }
    }
    else
    {
        emit LOG_E("", false, true);
        emit LOG_E("No valid response from ECU", true, true);

        return STATUS_ERROR;
    }

    emit LOG_I(" erased", false, true);

    msg = QString("Start flash write addr: 0x%1 len: 0x%2").arg(block_start, 8, 16, QLatin1Char('0')).arg(block_len, 8, 16, QLatin1Char('0')).toUtf8();
    emit LOG_I(msg, true, true);

    timer.start();
    while (remain)
    {
        if (stopRequested())
        {
            return STATUS_ERROR;
        }

        datalen = blocksize + 4; // 0x200 + 4
        output.clear();
        output.append((uint8_t)0x00);
        output.append((uint8_t)0x00);
        output.append((uint8_t)0x07);
        output.append((uint8_t)0xE1);
        output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
        output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
        output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
        output.append((uint8_t)(datalen + 1) & 0xFF);
        output.append((uint8_t)(SUB_KERNEL_WRITE_FLASH_BUFFER & 0xFF));
        output.append((uint8_t)(start >> 24) & 0xFF);
        output.append((uint8_t)(start >> 16) & 0xFF);
        output.append((uint8_t)(start >> 8) & 0xFF);
        output.append((uint8_t)start & 0xFF);
        for (unsigned int i = start; i < (start + blocksize); i++)
        {
            output.append(src[i]);
        }
        serial->write_serial_data_echo_check(output);
        //
        // delay(50);
        received = serial->read_serial_data(serial_read_long_timeout);
        if (received.length() > 8)
        {
            if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_WRITE_FLASH_BUFFER | 0x40))
            {
                emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

                return STATUS_ERROR;
            }
        }
        else
        {
            emit LOG_E("No valid response from ECU", true, true);

            return STATUS_ERROR;
        }

        emit LOG_D("Data written to flash buffer", true, true);

        QString start_address = QString("%1").arg(start, 8, 16, QLatin1Char('0')).toUpper();
        msg = QString("Write flash buffer: 0x%1 (%2\% - %3 B/s, ~ %4 s)").arg(start_address).arg((unsigned)100 * (len - remain) / len, 1, 10, QLatin1Char('0')).arg((uint32_t)curspeed, 1, 10, QLatin1Char('0')).arg(tleft, 1, 10, QLatin1Char('0')).toUtf8();
        emit LOG_I(msg, true, true);

        remain -= blocksize;
        start += blocksize;
        byteindex += blocksize;
        // src += blocksize;

        chrono = timer.elapsed();
        timer.start();

        if (!chrono)
        {
            chrono += 1;
        }
        curspeed = blocksize * (1000.0f / chrono); // avg B/s
        if (!curspeed)
        {
            curspeed += 1;
        }

        tleft = ((float)flashbytescount - byteindex) / curspeed; // s
        if (tleft > 9999)
        {
            tleft = 9999;
        }
        tleft++;

        float pleft = (float)byteindex / (float)flashbytescount * 100.0f;
        emit progressChanged(pleft);

        if ((flashblockstart + flashblocksize) == start)
        {
            emit LOG_I("Flash buffer write complete... ", true, true);
            imgcrc32 = SsmProtocol::crc32(&src[flashblockstart], flashblocksize);
            emit LOG_D("Image CRC32: 0x" + QString::number(imgcrc32, 16), true, true);

            uint8_t SUB_KERNEL_CMD = 0;
            if (test_write)
            {
                SUB_KERNEL_CMD = (uint8_t)(SUB_KERNEL_VALIDATE_FLASH_BUFFER & 0xFF);
                emit LOG_I("Validate flash addr: 0x" + QString::number(flashblockstart, 16), true, false);
                emit LOG_I(" len: 0x" + QString::number(flashblocksize, 16), false, false);
                emit LOG_I(" crc32: 0x" + QString::number(imgcrc32, 16), false, true);
            }
            else if (!test_write)
            {
                SUB_KERNEL_CMD = (uint8_t)(SUB_KERNEL_COMMIT_FLASH_BUFFER & 0xFF);
                emit LOG_I("Committ flash addr: 0x" + QString::number(flashblockstart, 16), true, false);
                emit LOG_I(" len: 0x" + QString::number(flashblocksize, 16), false, false);
                emit LOG_I(" crc32: 0x" + QString::number(imgcrc32, 16), false, true);
            }

            datalen = 10;
            output.clear();
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x07);
            output.append((uint8_t)0xE1);
            output.append((uint8_t)((SUB_KERNEL_START_COMM >> 8) & 0xFF));
            output.append((uint8_t)(SUB_KERNEL_START_COMM & 0xFF));
            output.append((uint8_t)((datalen + 1) >> 8) & 0xFF);
            output.append((uint8_t)(datalen + 1) & 0xFF);
            output.append((uint8_t)(SUB_KERNEL_CMD & 0xFF));
            output.append((uint8_t)(flashblockstart >> 24) & 0xFF);
            output.append((uint8_t)(flashblockstart >> 16) & 0xFF);
            output.append((uint8_t)(flashblockstart >> 8) & 0xFF);
            output.append((uint8_t)flashblockstart & 0xFF);
            output.append((uint8_t)(flashblocksize >> 8) & 0xFF);
            output.append((uint8_t)flashblocksize & 0xFF);
            output.append((uint8_t)(imgcrc32 >> 24) & 0xFF);
            output.append((uint8_t)(imgcrc32 >> 16) & 0xFF);
            output.append((uint8_t)(imgcrc32 >> 8) & 0xFF);
            output.append((uint8_t)imgcrc32 & 0xFF);
            received = serial->write_serial_data_echo_check(output);

            // delay(200);
            received = serial->read_serial_data(serial_read_extra_long_timeout);

            if (received.length() > 7)
            {
                if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) || (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || (uint8_t)received.at(8) != (SUB_KERNEL_CMD + 0x40))
                {
                    emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(8, received.length() - 1)), true, true);

                    return STATUS_ERROR;
                }
            }
            else
            {
                emit LOG_E("No valid response from ECU", true, true);

                return STATUS_ERROR;
            }
            flashblockstart += flashblocksize;
        }
    }
    return STATUS_SUCCESS;
}

/*
 * Generate denso can seed key from received seed bytes
 *
 * @return seed key (4 bytes)
 */
QByteArray FlashTcuSubaruDensoSH705xCanOperation::generate_seed_key(const QByteArray& requested_seed)
{
    QByteArray key;

    const uint16_t keytogenerateindex_1[] = {
        0x78B1, 0x4625, 0x201C, 0x9EA5,
        0xAD6B, 0x35F4, 0xFD21, 0x5E71,
        0xB046, 0x7F4A, 0x4B75, 0x93F9,
        0x1895, 0x8961, 0x3ECC, 0x862B};

    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

    key = SsmProtocol::calculateSeedKey(requested_seed, keytogenerateindex_1, indextransformation);

    return key;
}

/*
 * Calculate denso seed key from received seed bytes
 *
 * @return seed key (4 bytes)
 */
/*
 * Encrypt upload data
 *
 * @return encrypted data
 */
QByteArray FlashTcuSubaruDensoSH705xCanOperation::encrypt_payload(const QByteArray& buf, uint32_t len)
{
    QByteArray encrypted;

    uint16_t keytogenerateindex[] = {
        0xC85B, 0x32C0, 0xE282, 0x92A0};

    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

    encrypted = SsmProtocol::calculatePayload(buf, len, keytogenerateindex, indextransformation);

    return encrypted;
}

QByteArray FlashTcuSubaruDensoSH705xCanOperation::decrypt_payload(const QByteArray& buf, uint32_t len)
{
    QByteArray decrypt;

    uint16_t keytogenerateindex[] = {
        0x92A0, 0xE282, 0x32C0, 0xC85B};

    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

    decrypt = SsmProtocol::calculatePayload(buf, len, keytogenerateindex, indextransformation);

    return decrypt;
}

/*
 * Request kernel id
 *
 * @return kernel id
 */
QByteArray FlashTcuSubaruDensoSH705xCanOperation::request_kernel_id()
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;
    QByteArray kernelid;

    request_denso_kernel_id = true;

    output.clear();

    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    output.append((uint8_t)0xE1);
    output.append((uint8_t)SUB_DENSOCAN_START_COMM);
    output.append((uint8_t)0xA0);

    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);

    int loop = 0;
    while (loop < 5 && received == "")
    {
        received = serial->write_serial_data_echo_check(output);
        emit LOG_I("Kernel ID request: " + SsmProtocol::toHex(output), true, true);
        delay(100);
        kernelid.clear();

        received = serial->read_serial_data(serial_read_long_timeout);
        emit LOG_I("Kernel ID response: " + SsmProtocol::toHex(received), true, true);
        received.remove(0, 2);
        kernelid = received;

        while (received != "")
        {
            received = serial->read_serial_data(serial_read_short_timeout);
            received.remove(0, 2);
            kernelid.append(received);
        }
        loop++;
    }
    request_denso_kernel_id = false;

    return kernelid;
}

/*
 * Add SSM header to message
 *
 * @return parsed message
 */
/*
 * Calculate SSM checksum to message
 *
 * @return 8-bit checksum
 */
/*
 * Parse QByteArray to readable form
 *
 * @return parsed message
 */
