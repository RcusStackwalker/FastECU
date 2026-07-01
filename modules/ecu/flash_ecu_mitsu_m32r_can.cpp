#include "flash_ecu_mitsu_m32r_can.h"
#include "serial_port_actions.h"
#include "protocol/mitsu_colt_can_protocol.h"

FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::EcuOperationsWindow)
    , ecuCalDef(ecuCalDef)
    , cmd_type(cmd_type)
{
    ui->setupUi(this);

    if (cmd_type == "write")
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to ECU");
    else if (cmd_type == "read")
        this->setWindowTitle("Read ROM from ECU");

    this->serial = serial;
}

FlashEcuMitsuM32rCan::~FlashEcuMitsuM32rCan()
{
    delete ui;
}

void FlashEcuMitsuM32rCan::closeEvent(QCloseEvent *event)
{
    kill_process = true;
}

void FlashEcuMitsuM32rCan::run()
{
    this->show();

    int result = STATUS_ERROR;
    set_progressbar_value(0);

    mcu_type_string = ecuCalDef->McuType;
    mcu_type_index = 0;
    while (flashdevices[mcu_type_index].name != 0)
    {
        if (flashdevices[mcu_type_index].name == mcu_type_string)
            break;
        mcu_type_index++;
    }
    emit LOG_D("MCU type: " + QString(flashdevices[mcu_type_index].name) + " index: " + QString::number(mcu_type_index), true, true);

    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(true);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("500000");
    serial->set_iso15765_source_address(0x7E0);
    serial->set_iso15765_destination_address(0x7E8);
    serial->open_serial_port();

    int ret = QMessageBox::warning(this, tr("Connecting to ECU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to ECU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
        case QMessageBox::Ok:
            emit LOG_I("Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...", true, true);
            result = connect_bootloader();

            if (result == STATUS_SUCCESS)
            {
                if (cmd_type == "read")
                {
                    emit external_logger("Reading ROM, please wait...");
                    emit LOG_I("Reading ROM from ECU using CAN", true, true);
                    result = read_mem(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
                }
                else if (cmd_type == "write")
                {
                    emit external_logger("Writing ROM, please wait...");
                    emit LOG_I("Writing ROM to ECU using CAN", true, true);
                    result = write_mem(false);
                }
            }
            emit external_logger("Finished");

            if (result == STATUS_SUCCESS)
            {
                QMessageBox::information(this, tr("ECU Operation"), "ECU operation was succesful, press OK to exit");
                this->close();
            }
            else
            {
                QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, press OK to exit and try again");
            }
            break;
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

QByteArray FlashEcuMitsuM32rCan::build_request(const QByteArray &sidPayload)
{
    QByteArray output;
    output.append(char(0x00));
    output.append(char(0x00));
    output.append(char(0x07));
    output.append(char(0xE0));
    output.append(sidPayload);
    return output;
}

/*
 * Connect to the Mitsubishi Colt CZT (Z37A) M32R CAN bootloader.
 * Read only needs the basic diagnostic session; write needs the bootload
 * session plus security access (matches
 * externals/livemonitor/obdsessionwidget.cpp's session-id selection and
 * obdengine.cpp's ObdSessionInitCommandSequence).
 *
 * @return success
 */
int FlashEcuMitsuM32rCan::connect_bootloader()
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QString msg;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    bool needSecurity = (cmd_type == "write");
    quint8 sessionId = needSecurity ? kSessionBootload : kSessionBasic;

    emit LOG_I("Starting diagnostic session...", true, true);
    output = build_request(buildDiagnosticSessionFrame(sessionId));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 5 || (uint8_t)received.at(4) != (kServiceDiagnosticSession + 0x40) || (uint8_t)received.at(5) != sessionId)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Diagnostic session ok", true, true);

    if (!needSecurity)
        return STATUS_SUCCESS;

    emit LOG_I("Requesting security seed...", true, true);
    output = build_request(buildSecurityAccessSeedRequestFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 9 || (uint8_t)received.at(4) != (kServiceSecurityAccess + 0x40) || (uint8_t)received.at(5) != 5)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }

    QByteArray seed = received.mid(6, 4);
    msg = parse_message_to_hex(seed);
    emit LOG_I("Received seed: " + msg, true, true);

    QByteArray key = seedKey(seed);
    msg = parse_message_to_hex(key);
    emit LOG_I("Calculated seed key: " + msg, true, true);

    emit LOG_I("Sending seed key to ECU...", true, true);
    output = build_request(buildSecurityAccessKeyFrame(key));
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 5 || (uint8_t)received.at(4) != (kServiceSecurityAccess + 0x40) || (uint8_t)received.at(5) != 6)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Security access ok", true, true);

    return STATUS_SUCCESS;
}

/*
 * Read ROM from the Colt CZT bootloader via ReadMemoryByAddress(0x23),
 * kFlashReadBlockSize-byte blocks. No security session required for read
 * (matches externals/livemonitor/obdsessionwidget.cpp's requestReadFlashBlock,
 * which only needs ObdEngine::SessionBasic).
 *
 * @return success
 */
int FlashEcuMitsuM32rCan::read_mem(uint32_t start_addr, uint32_t length)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QByteArray romdata;

    uint32_t addr = start_addr;
    uint32_t end_addr = start_addr + length;

    set_progressbar_value(0);
    emit LOG_I("Start reading ROM, please wait...", true, true);

    while (addr < end_addr)
    {
        if (kill_process)
            return STATUS_ERROR;

        output = build_request(buildReadMemoryByAddressFrame(addr, (quint8)kFlashReadBlockSize));
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);

        if (received.length() < int(4 + 1 + kFlashReadBlockSize) || (uint8_t)received.at(4) != (kServiceReadMemoryByAddress + 0x40))
        {
            emit LOG_E("Wrong response from ECU at 0x" + QString::number(addr, 16) + ": " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return STATUS_ERROR;
        }

        romdata.append(received.mid(5, int(kFlashReadBlockSize)));
        addr += kFlashReadBlockSize;

        float pleft = (float)(addr - start_addr) / (float)length * 100.0f;
        set_progressbar_value((int)pleft);
    }

    emit LOG_I("ROM read complete", true, true);
    ecuCalDef->FullRomData = romdata;
    set_progressbar_value(100);

    return STATUS_SUCCESS;
}

/*
 * RequestDownload + chunked TransferData of `data` to `start`, followed by
 * a RequestDownload + TransferData of its checksum to the fixed
 * kCrcTransferAddress, then RoutineControl(225) to make the ECU verify and
 * commit the transfer. Mirrors ObdFlashWriteCommandSequence in
 * externals/livemonitor/obdengine.cpp exactly — every upload in that
 * protocol (the erase-page blob, the write-page blob, and the final ROM
 * data) goes through this same five-step sequence.
 *
 * @return success
 */
bool FlashEcuMitsuM32rCan::upload_and_commit(uint32_t start, const QByteArray &data)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;

    output = build_request(buildRequestDownloadFrame(start, data.size()));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestDownload + 0x40))
    {
        emit LOG_E("RequestDownload to 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    const QVector<QByteArray> chunks = buildTransferDataFrames(data);
    for (const QByteArray &chunk : chunks)
    {
        output = build_request(chunk);
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);
        if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceTransferData + 0x40))
        {
            emit LOG_E("TransferData to 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return false;
        }
    }

    output = build_request(buildRequestDownloadFrame(kCrcTransferAddress, kCrcTransferSize));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestDownload + 0x40))
    {
        emit LOG_E("RequestDownload for checksum rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    quint16 crc = checksum(data);
    QByteArray crcData;
    crcData.append(char((crc >> 8) & 0xFF));
    crcData.append(char(crc & 0xFF));
    output = build_request(buildTransferDataFrames(crcData).first());
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceTransferData + 0x40))
    {
        emit LOG_E("TransferData for checksum rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    output = build_request(buildRoutineCheckCrcFrame(start));
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("RoutineControl CRC check for 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    return true;
}

/*
 * Write the patched ROM's userspace region (0x8000-0x60000) to the Colt
 * CZT bootloader. Four stages, ported from
 * externals/livemonitor/obdsessionwidget.cpp's
 * erasePageUploadedCallback/writePageUploadedCallback/flashErasedCallback
 * chain: upload erase-page routine, upload write-page routine, trigger
 * erase, upload ROM data.
 *
 * test_write is not implemented for this protocol (matches every other
 * M32R module in FastECU; see config/protocols.cfg's test_write=no).
 *
 * @return success
 */
int FlashEcuMitsuM32rCan::write_mem(bool test_write)
{
    using namespace MitsuColtCan;
    Q_UNUSED(test_write);

    QByteArray romdata = ecuCalDef->FullRomData;
    if ((uint32_t)romdata.size() < kUserspaceEnd)
    {
        emit LOG_E("ROM file too small: need at least 0x" + QString::number(kUserspaceEnd, 16) + " bytes", true, true);
        return STATUS_ERROR;
    }

    emit LOG_I("Uploading erase-page routine to RAM 0x" + QString::number(kEraseRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kEraseRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kErasePageRoutine), sizeof(kErasePageRoutine))))
    {
        emit LOG_E("Erase-page routine upload failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Erase page uploaded", true, true);

    emit LOG_I("Uploading write-page routine to RAM 0x" + QString::number(kWriteRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kWriteRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kWritePageRoutine), sizeof(kWritePageRoutine))))
    {
        emit LOG_E("Write-page routine upload failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Write page uploaded", true, true);

    // --- HIGH RISK STEP ---
    // This 12-byte ServiceRequestReflash(0x3B) payload is carried over verbatim
    // from externals/livemonitor/obdsessionwidget.cpp:180-181, where the
    // original author's own comment reads "caused bootloader lockup" during
    // their testing. Only attempt this on a bench/spare ECU with a recovery
    // path available (see this project's boot-talk utility for bricked-ECU
    // recovery). See docs/superpowers/specs/2026-06-30-fastecu-colt-can-reflash-design.md
    // for full context.
    int reflashConfirm = QMessageBox::warning(this, tr("Erase trigger"),
        tr("About to send the flash-erase trigger command. This exact "
           "sequence is known to have locked up the bootloader during the "
           "original implementation's testing. Only continue if this is a "
           "bench/spare ECU with a recovery path available.\n\nContinue?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reflashConfirm != QMessageBox::Yes)
    {
        emit LOG_I("Erase trigger canceled by user", true, true);
        return STATUS_ERROR;
    }

    QByteArray output = build_request(buildRequestReflashUnlockFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    QByteArray received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestReflash + 0x40))
    {
        emit LOG_E("Reflash unlock rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }

    output = build_request(buildRoutineEraseFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("Erase trigger rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Userspace flash erased", true, true);

    emit LOG_I("Writing ROM userspace 0x" + QString::number(kUserspaceStart, 16) + "-0x" + QString::number(kUserspaceEnd, 16) + "...", true, true);
    QByteArray userspace = romdata.mid(int(kUserspaceStart), int(kUserspaceEnd - kUserspaceStart));
    if (!upload_and_commit(kUserspaceStart, userspace))
    {
        emit LOG_E("ROM userspace write failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Userspace flash written", true, true);

    return STATUS_SUCCESS;
}

QString FlashEcuMitsuM32rCan::parse_message_to_hex(QByteArray received)
{
    QString msg;
    for (int i = 0; i < received.length(); i++)
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    return msg;
}

void FlashEcuMitsuM32rCan::set_progressbar_value(int value)
{
    bool valueChanged = true;
    if (ui->progressbar)
    {
        valueChanged = ui->progressbar->value() != value;
        ui->progressbar->setValue(value);
    }
    if (valueChanged)
        emit external_logger(value);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

void FlashEcuMitsuM32rCan::delay(int timeout)
{
    QTime dieTime = QTime::currentTime().addMSecs(timeout);
    while (QTime::currentTime() < dieTime)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
}
