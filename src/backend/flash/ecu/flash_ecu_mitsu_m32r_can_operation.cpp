#include "src/backend/flash/ecu/flash_ecu_mitsu_m32r_can_operation.h"
#include "src/backend/flash/flash_utils.h"
#include "src/algorithms/protocol/ssm/ssm_protocol.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/definitions/kernelmemorymodels.h"

#include <utility>

FlashEcuMitsuM32rCanOperation::FlashEcuMitsuM32rCanOperation(
    SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
    QString cmd_type, QWidget *dialog, bool useVendorChallenge, QObject *parent,
    PromptFn promptOverride)
    : FlashOperationWorker(dialog, parent, std::move(promptOverride)), serial(serial), ecuCalDef(ecuCalDef), cmd_type(std::move(cmd_type)), useVendorChallenge(useVendorChallenge)
{
}

bool FlashEcuMitsuM32rCanOperation::execute()
{
    mcu_type_string = ecuCalDef->McuType;
    mcu_type_index = FlashUtils::findFlashDeviceIndex(mcu_type_string);
    if (mcu_type_index < 0)
    {
        emit LOG_E("Unknown MCU type: " + mcu_type_string, true, true);
        return false;
    }
    emit LOG_D("MCU type: " + QString(flashdevices[mcu_type_index].name) + " index: " + QString::number(mcu_type_index), true, true);

    FlashUtils::configureIso15765Can(serial, "500000", 0x7E0, 0x7E8);
    serial->open_serial_port();

    emit LOG_I("Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...", true, true);
    int result = connect_bootloader();

    if (result == STATUS_SUCCESS)
    {
        if (cmd_type == "read")
        {
            emit externalLoggerMessage("Reading ROM, please wait...");
            emit LOG_I("Reading ROM from ECU using CAN", true, true);
            result = read_mem(flashdevices[mcu_type_index].fblocks[0].start,
                              flashdevices[mcu_type_index].fblocks[0].len);
        }
        else if (cmd_type == "write")
        {
            emit externalLoggerMessage("Writing ROM, please wait...");
            emit LOG_I("Writing ROM to ECU using CAN", true, true);
            result = write_mem(false);
        }
    }

    return result == STATUS_SUCCESS;
}

QByteArray FlashEcuMitsuM32rCanOperation::build_request(const QByteArray& sidPayload)
{
    return FlashUtils::buildIso15765Request(0x7E0, sidPayload);
}

int FlashEcuMitsuM32rCanOperation::connect_bootloader()
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

    const bool needVendorChallenge = useVendorChallenge;
    const bool needFactorySecurity = (cmd_type == "write");
    quint8 sessionId = needFactorySecurity ? kSessionBootload : kSessionBasic;

    if (needVendorChallenge)
    {
        emit LOG_I("Requesting vendor extension challenge seed...", true, true);
        output = build_request(MitsuColtCanVendorExt::buildChallengeSeedRequestFrame());
        serial->write_serial_data_echo_check(output);
        delay(200);
        received = serial->read_serial_data(serial_read_timeout);
        if (received.length() <= 10 || (uint8_t)received.at(4) != (MitsuColtCanVendorExt::kServiceReadMemoryByAddress + 0x40) || (uint8_t)received.at(5) != MitsuColtCanVendorExt::kVendorChallengeSelector || (uint8_t)received.at(6) != MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction)
        {
            emit LOG_E("Wrong vendor challenge response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return STATUS_ERROR;
        }

        QByteArray vendorSeed = received.mid(7, 4);
        msg = SsmProtocol::toHex(vendorSeed);
        emit LOG_I("Received vendor seed: " + msg, true, true);

        quint32 vendorKey = MitsuColtCanVendorExt::challengeInverseTransform(MitsuColtCanVendorExt::bytesToSeed(vendorSeed));
        QByteArray vendorKeyBytes = MitsuColtCanVendorExt::keyToBytes(vendorKey);
        msg = SsmProtocol::toHex(vendorKeyBytes);
        emit LOG_I("Calculated vendor key: " + msg, true, true);

        emit LOG_I("Sending vendor key to ECU...", true, true);
        output = build_request(MitsuColtCanVendorExt::buildChallengeKeyFrame(vendorKey));
        serial->write_serial_data_echo_check(output);
        delay(200);
        received = serial->read_serial_data(serial_read_timeout);
        if (received.length() <= 6 || (uint8_t)received.at(4) != (MitsuColtCanVendorExt::kServiceReadMemoryByAddress + 0x40) || (uint8_t)received.at(5) != MitsuColtCanVendorExt::kVendorChallengeSelector || (uint8_t)received.at(6) != MitsuColtCanVendorExt::kVendorChallengeKeySubfunction)
        {
            emit LOG_E("Vendor challenge key rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return STATUS_ERROR;
        }
        emit LOG_I("Vendor challenge accepted", true, true);
    }

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

    if (!needFactorySecurity)
    {
        return STATUS_SUCCESS;
    }

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
    msg = SsmProtocol::toHex(seed);
    emit LOG_I("Received seed: " + msg, true, true);

    QByteArray key = seedKey(seed);
    msg = SsmProtocol::toHex(key);
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

bool FlashEcuMitsuM32rCanOperation::readFlashRange(uint32_t start_addr, uint32_t length, QByteArray *outData)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QByteArray data;

    uint32_t addr = start_addr;
    uint32_t end_addr = start_addr + length;

    while (addr < end_addr)
    {
        if (stopRequested())
        {
            return false;
        }

        const uint32_t remaining = end_addr - addr;
        const quint8 chunkLen = (quint8)((remaining < kFlashReadBlockSize) ? remaining : kFlashReadBlockSize);

        output = build_request(buildReadMemoryByAddressFrame(addr, chunkLen));
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);

        if (received.length() < int(4 + 1 + chunkLen) || (uint8_t)received.at(4) != (kServiceReadMemoryByAddress + 0x40))
        {
            emit LOG_E("Wrong response from ECU at 0x" + QString::number(addr, 16) + ": " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return false;
        }

        data.append(received.mid(5, int(chunkLen)));
        addr += chunkLen;

        float pleft = (float)(addr - start_addr) / (float)length * 100.0f;
        emit progressChanged((int)pleft);
    }

    *outData = data;
    return true;
}

int FlashEcuMitsuM32rCanOperation::read_mem(uint32_t start_addr, uint32_t length)
{
    emit progressChanged(0);
    emit LOG_I("Start reading ROM, please wait...", true, true);

    QByteArray romdata;
    if (!readFlashRange(start_addr, length, &romdata))
    {
        return STATUS_ERROR;
    }

    emit LOG_I("ROM read complete", true, true);
    ecuCalDef->FullRomData = romdata;
    emit progressChanged(100);

    return STATUS_SUCCESS;
}

bool FlashEcuMitsuM32rCanOperation::upload_and_commit(uint32_t start, const QByteArray& data)
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
    for (const QByteArray& chunk : chunks)
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

bool FlashEcuMitsuM32rCanOperation::ensureTopRegionWritten(const QByteArray& romdata)
{
    using namespace MitsuColtCan;

    emit LOG_I("Checking top 128KB (0x" + QString::number(kTopRegionStart, 16) + "-0x" + QString::number(kTopRegionEnd, 16) + ")...", true, true);

    QByteArray currentTop;
    if (!readFlashRange(kTopRegionStart, kTopRegionLength, &currentTop))
    {
        return false;
    }

    const QByteArray wantedTop = romdata.mid(int(kTopRegionStart), int(kTopRegionLength));
    if (currentTop == wantedTop)
    {
        emit LOG_I("Top 128KB already matches, no bootstrap needed", true, true);
        return true;
    }

    emit LOG_I("Top 128KB mismatch, bootstrapping via redirect routines...", true, true);

    int bootstrapConfirm = confirm(tr("Top 128KB bootstrap"),
                                   tr("The top 128KB (0x60000-0x80000) does not match the ROM being "
                                      "written and needs a one-time bootstrap pass through custom "
                                      "erase/write redirect helpers, outside the range the vendor "
                                      "bootloader normally allows. This sends the same high-risk erase "
                                      "trigger sequence used for the main write, once now and once more "
                                      "for the main write that follows. Only continue on a bench/spare "
                                      "ECU with a recovery path available.\n\nContinue?"),
                                   QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (bootstrapConfirm != QMessageBox::Yes)
    {
        emit LOG_I("Top 128KB bootstrap canceled by user", true, true);
        return false;
    }

    emit LOG_I("Uploading erase redirect routine to RAM 0x" + QString::number(kEraseRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kEraseRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kEraseRedirectRoutine), sizeof(kEraseRedirectRoutine))))
    {
        emit LOG_E("Erase redirect routine upload failed", true, true);
        return false;
    }

    emit LOG_I("Uploading write redirect routine to RAM 0x" + QString::number(kWriteRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kWriteRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kWriteRedirectRoutine), sizeof(kWriteRedirectRoutine))))
    {
        emit LOG_E("Write redirect routine upload failed", true, true);
        return false;
    }

    QByteArray output = build_request(buildRequestReflashUnlockFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    QByteArray received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestReflash + 0x40))
    {
        emit LOG_E("Reflash unlock (top 128KB bootstrap) rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    output = build_request(buildRoutineEraseFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("Erase trigger (top 128KB bootstrap) rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }
    emit LOG_I("Carrier window erased", true, true);

    if (!upload_and_commit(kUserspaceStart, wantedTop))
    {
        emit LOG_E("Top 128KB redirect write failed", true, true);
        return false;
    }
    emit LOG_I("Top 128KB written via redirect", true, true);

    QByteArray verifyTop;
    if (!readFlashRange(kTopRegionStart, kTopRegionLength, &verifyTop))
    {
        return false;
    }
    if (verifyTop != wantedTop)
    {
        emit LOG_E("Top 128KB verify failed after redirect write", true, true);
        return false;
    }
    emit LOG_I("Top 128KB verified", true, true);

    return true;
}

int FlashEcuMitsuM32rCanOperation::write_mem(bool test_write)
{
    using namespace MitsuColtCan;
    Q_UNUSED(test_write);

    QByteArray romdata = ecuCalDef->FullRomData;
    if ((uint32_t)romdata.size() < kTopRegionEnd)
    {
        emit LOG_E("ROM file too small: need at least 0x" + QString::number(kTopRegionEnd, 16) + " bytes", true, true);
        return STATUS_ERROR;
    }

    if (!ensureTopRegionWritten(romdata))
    {
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
    // recovery).
    // for full context.
    int reflashConfirm = confirm(tr("Erase trigger"),
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
