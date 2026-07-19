#include "src/backend/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.h"

#include <utility>
#include "src/backend/flash/flash_utils.h"
#include "src/algorithms/protocol/ssm/ssm_protocol.h"
#include "serial_port_actions.h"

FlashTcuSubaruHitachiM32rKlineOperation::FlashTcuSubaruHitachiM32rKlineOperation(
    SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
    QString cmd_type, QWidget *dialog, QObject *parent, PromptFn promptOverride)
    : FlashOperationWorker(dialog, parent, std::move(promptOverride)), serial(serial), ecuCalDef(ecuCalDef), cmd_type(std::move(cmd_type))
{
}

bool FlashTcuSubaruHitachiM32rKlineOperation::execute()
{
    int result = STATUS_ERROR;

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

    if (cmd_type == "read")
    {
        emit LOG_I("Read memory with flashmethod '" + flash_method + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
    }
    else if (cmd_type == "test_write")
    {
        test_write = true;
        emit LOG_I("Test write memory with flashmethod '" + flash_method + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
    }
    else if (cmd_type == "write")
    {
        test_write = false;
        emit LOG_I("Write memory with flashmethod '" + flash_method + "' and kernel '" + ecuCalDef->Kernel + "'", true, true);
    }

    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(false);
    serial->set_is_iso14230_connection(true);
    serial->open_serial_port();
    serial->change_port_speed("4800");
    serial->set_add_iso14230_header(false);
    tester_id = 0xF0;
    target_id = 0x18;

    emit LOG_I("Connecting to Subaru TCU Hitachi CAN bootloader, please wait...", true, true);
    result = connect_bootloader();

    if (result == STATUS_SUCCESS)
    {
        if (cmd_type == "read")
        {
            emit externalLoggerMessage("Reading ROM, please wait...");
            emit LOG_I("Reading ROM from TCU using K-Line", true, true);
            result = read_a0_rom(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
            // result = read_a0_ram_subaru_tcu_hitachi_kline(0x80000, 0x100000);
            // result = read_b8_subaru_tcu_hitachi_kline(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
            // result = read_b0_subaru_tcu_hitachi_kline(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
        }
        else if (cmd_type == "test_write" || cmd_type == "write")
        {
            emit externalLoggerMessage("Writing ROM, please wait...");
            emit LOG_I("Not yet implemented: Writing ROM to TCU Subaru Hitachi using CAN", true, true);
            // result = write_mem_subaru_denso_can_02_32bit(test_write);
        }
    }

    return result == STATUS_SUCCESS;
}

/*
 * Connect to Subaru TCU Hitachi CAN bootloader
 *
 * @return success
 */
int FlashTcuSubaruHitachiM32rKlineOperation::connect_bootloader()
{
    QByteArray output;
    QByteArray received;
    QByteArray seed;
    QByteArray seed_key;

    QString msg;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    delay(100);

    emit LOG_I("Initializing k-line communications", true, true);

    emit LOG_I("Requesting ECU ID", true, true);
    output.clear();
    output.append((uint8_t)0xBF);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xFF)
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

    received.remove(0, 8);
    received.remove(5, received.length() - 5);
    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUpper());
    }

    QString ecuid = msg;
    emit LOG_I("ECU ID: " + ecuid, true, true);
    if (cmd_type == "read")
    {
        ecuCalDef->RomId = ecuid + "_";
    }

    emit LOG_I("Requesting to start communication", true, true);
    output.clear();
    output.append((uint8_t)0x81);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xC1)
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
    emit LOG_I("Start communication ok", true, true);

    emit LOG_I("Requesting timings params", true, true);
    output.clear();
    output.append((uint8_t)0x83);
    output.append((uint8_t)0x00);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 4)
    {
        if ((uint8_t)received.at(4) != 0xC3)
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
    emit LOG_I("Timing parameters ok", true, true);

    emit LOG_I("Requesting seed", true, true);
    output.clear();
    output.append((uint8_t)0x27);
    output.append((uint8_t)0x01);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 9)
    {
        if ((uint8_t)received.at(4) != 0x67 || (uint8_t)received.at(5) != 0x01)
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
    emit LOG_I("Seed request ok", true, true);

    seed.append(received.at(6));
    seed.append(received.at(7));
    seed.append(received.at(8));
    seed.append(received.at(9));

    msg = SsmProtocol::toHex(seed);
    emit LOG_I("Received seed: " + msg, true, true);

    seed_key = generate_kline_seed_key(seed);

    msg = SsmProtocol::toHex(seed_key);
    emit LOG_I("Calculated seed key: " + msg, true, true);

    emit LOG_I("Sending seed key to ECU", true, true);
    output.clear();
    output.append((uint8_t)0x27);
    output.append((uint8_t)0x02);
    output.append(seed_key);
    output = SsmProtocol::addHeader(output, tester_id, target_id, false);
    serial->write_serial_data_echo_check(output);

    received = serial->read_serial_data(serial_read_timeout);

    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != 0x67 || (uint8_t)received.at(5) != 0x02)
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
    emit LOG_I("Seed key ok", true, true);
    /*
        emit LOG_I("Set session mode", true, true);
        output.clear();
        output.append((uint8_t)0x10);
        output.append((uint8_t)0x85);
        output.append((uint8_t)0x02);
        output = SsmProtocol::addHeader(output, tester_id, target_id, false);
        serial->write_serial_data_echo_check(output);

        received = serial->read_serial_data(serial_read_timeout);

        if (received.length() > 4)
        {
            if ((uint8_t)received.at(4) != 0x50)
            {
                emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length()-1)), true, true);

                return STATUS_ERROR;
            }
        }
        else
        {
            emit LOG_E("No valid response from ECU", true, true);

            return STATUS_ERROR;
        }
        emit LOG_I("Succesfully set to programming session", true, true);
    */
    return STATUS_SUCCESS;
}

/*
 * For reading a portion of ROM using a0 command
 *
 *
 */
int FlashTcuSubaruHitachiM32rKlineOperation::read_a0_rom(uint32_t start_addr, uint32_t length)
{

    QByteArray received;
    QByteArray msg;
    QByteArray mapdata;

    uint32_t curr_addr, block_count, num_blocks;
    uint32_t tail_size, block_len, loop_ctr, block_size;

    block_size = 96;
    // length = 0x400;   // hack for testing

    emit LOG_I("Commencing TCU ROM read", true, true);

    curr_addr = start_addr; //- 0x00100000;          // manual adjustment until fix processor details
    num_blocks = length / block_size;
    tail_size = length % block_size;
    if (tail_size != 0)
    {
        num_blocks++;
    }
    else
    {
        tail_size = block_size;
    }

    for (block_count = 0; block_count < num_blocks; block_count++)
    {
        if (block_count == (num_blocks - 1))
        {
            block_len = tail_size;
        }
        else
        {
            block_len = block_size;
        }

        msg = QString("ROM read addr: %1 length: %2").arg(curr_addr).arg(block_len).toUtf8();
        emit LOG_I(msg, true, true);

        for (loop_ctr = 0; loop_ctr < 5; loop_ctr++)
        {
            received = send_sid_a0_block_read(curr_addr, block_len - 1);
            if (received.length() > 5)
            {
                break;
            }
        }

        if (received.length() > 5)
        {
            received.remove(0, 5);
            mapdata.append(received, (received.length() - 1));
        }

        curr_addr += block_len;
    }

    ecuCalDef->FullRomData = mapdata;

    return STATUS_SUCCESS;
}

/*
 * For reading a portion of ROM using b8 command
 *
 *
 */
int FlashTcuSubaruHitachiM32rKlineOperation::read_b8(uint32_t start_addr, uint32_t length)
{

    QByteArray received;
    QByteArray msg;
    QByteArray mapdata;

    uint32_t curr_addr, block_count, num_blocks;
    uint32_t tail_size, block_len, loop_ctr, block_size;

    block_size = 1;
    length = 0x10; // hack for testing

    emit LOG_I("Commencing TCU ROM read", true, true);

    curr_addr = start_addr - 0x00100000; // manual adjustment until fix processor details
    num_blocks = length / block_size;
    tail_size = length % block_size;
    if (tail_size != 0)
    {
        num_blocks++;
    }
    else
    {
        tail_size = block_size;
    }

    for (block_count = 0; block_count < num_blocks; block_count++)
    {
        if (block_count == (num_blocks - 1))
        {
            block_len = tail_size;
        }
        else
        {
            block_len = block_size;
        }

        msg = QString("ROM read addr: %1 length: %2").arg(curr_addr).arg(block_len).toUtf8();
        emit LOG_I(msg, true, true);

        for (loop_ctr = 0; loop_ctr < 5; loop_ctr++)
        {
            received = send_sid_b8_byte_read(curr_addr);
            if (received.length() > 5)
            {
                break;
            }
        }

        if (received.length() > 5)
        {
            received.remove(0, 5);
            mapdata.append(received, (received.length() - 1));
        }

        curr_addr += block_len;
    }

    ecuCalDef->FullRomData = mapdata;

    return STATUS_SUCCESS;
}

/*
 * For reading a portion of ROM using b0 command
 *
 *
 */
int FlashTcuSubaruHitachiM32rKlineOperation::read_b0(uint32_t start_addr, uint32_t length)
{

    QByteArray received;
    QByteArray msg;
    QByteArray mapdata;

    uint32_t curr_addr, block_count, num_blocks;
    uint32_t tail_size, block_len, loop_ctr, block_size;

    block_size = 96;
    length = 4; // for testing

    emit LOG_I("Commencing TCU ROM read", true, true);

    curr_addr = 0x804ffc; // for testing
    num_blocks = length / block_size;
    tail_size = length % block_size;
    if (tail_size != 0)
    {
        num_blocks++;
    }
    else
    {
        tail_size = block_size;
    }

    for (block_count = 0; block_count < num_blocks; block_count++)
    {
        if (block_count == (num_blocks - 1))
        {
            block_len = tail_size;
        }
        else
        {
            block_len = block_size;
        }

        msg = QString("ROM read addr: %1 length: %2").arg(curr_addr).arg(block_len).toUtf8();
        emit LOG_I(msg, true, true);

        for (loop_ctr = 0; loop_ctr < 5; loop_ctr++)
        {
            received = send_sid_b0_block_write(curr_addr, block_len);
            if (received.length() > 5)
            {
                break;
            }
        }

        if (received.length() > 5)
        {
            received.remove(0, 5);
            mapdata.append(received, (received.length() - 1));
        }

        curr_addr += block_len;
    }

    ecuCalDef->FullRomData = mapdata;

    return STATUS_SUCCESS;
}

/*
 * For reading a portion of RAM
 *
 *
 */
int FlashTcuSubaruHitachiM32rKlineOperation::read_a0_ram(uint32_t start_addr, uint32_t length)
{

    QByteArray received;
    QByteArray msg;
    QByteArray mapdata;

    uint32_t curr_addr, block_count, num_blocks;
    uint32_t tail_size, block_len, loop_ctr, block_size;

    block_size = 96;
    length = 0x400; // hack for testing

    emit LOG_I("Commencing TCU ROM read", true, true);

    curr_addr = start_addr;
    num_blocks = length / block_size;
    tail_size = length % block_size;
    if (tail_size != 0)
    {
        num_blocks++;
    }
    else
    {
        tail_size = block_size;
    }

    for (block_count = 0; block_count < num_blocks; block_count++)
    {
        if (block_count == (num_blocks - 1))
        {
            block_len = tail_size;
        }
        else
        {
            block_len = block_size;
        }

        msg = QString("ROM read addr: %1 length: %2").arg(curr_addr).arg(block_len).toUtf8();
        emit LOG_I(msg, true, true);

        for (loop_ctr = 0; loop_ctr < 5; loop_ctr++)
        {
            received = send_sid_a0_block_read(curr_addr, block_len - 1);
            if (received.length() > 5)
            {
                break;
            }
        }

        if (received.length() > 5)
        {
            received.remove(0, 5);
            mapdata.append(received, (received.length() - 1));
        }

        curr_addr += block_len;
    }

    ecuCalDef->FullRomData = mapdata;

    return STATUS_SUCCESS;
}

/*
 * Generate denso kline seed key from received seed bytes
 *
 * @return seed key (4 bytes)
 */
QByteArray FlashTcuSubaruHitachiM32rKlineOperation::generate_kline_seed_key(const QByteArray& requested_seed)
{
    QByteArray key;

    const uint16_t keytogenerateindex[] = {
        0x0FE9, 0xCA58, 0x5E90, 0xDFF1,
        0x690B, 0xF591, 0x1794, 0x5C7B,
        0xA7BF, 0x98E5, 0x0B63, 0xA1C9,
        0x79BF, 0xF413, 0x82B1, 0xA895};

    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

    key = SsmProtocol::calculateSeedKey(requested_seed, keytogenerateindex, indextransformation);

    return key;
}

QByteArray FlashTcuSubaruHitachiM32rKlineOperation::send_sid_b0_block_write(uint32_t dataaddr, uint32_t datalen)
{
    QByteArray output;
    QByteArray received;
    int i;

    output.append((uint8_t)0xb0);
    output.append((uint8_t)(dataaddr >> 16) & 0xFF);
    output.append((uint8_t)(dataaddr >> 8) & 0xFF);
    output.append((uint8_t)dataaddr & 0xFF);
    output.append((uint8_t)0x80);
    output.append((uint8_t)0x18);
    output.append((uint8_t)0xf0);
    output.append((uint8_t)((datalen + 4) & 0xFF));

    serial->write_serial_data_echo_check(SsmProtocol::addHeader(output, tester_id, target_id, false));
    emit LOG_I("Sent: " + SsmProtocol::toHex(SsmProtocol::addHeader(output, tester_id, target_id, false)), true, true);
    delay(100);
    received = serial->read_serial_data(receive_timeout);
    // received = QByteArray::fromHex("80f018f8e0");
    delay(100);
    emit LOG_I("Recd: " + SsmProtocol::toHex(received), true, true);

    return received;
}

/*
 * SSM Byte Read (K-Line)
 *
 * @return received response
 */
QByteArray FlashTcuSubaruHitachiM32rKlineOperation::send_sid_b8_byte_read(uint32_t dataaddr)
{
    QByteArray output;
    QByteArray received;

    output.append((uint8_t)0xb8);
    output.append((uint8_t)(dataaddr >> 16) & 0xFF);
    output.append((uint8_t)(dataaddr >> 8) & 0xFF);
    output.append((uint8_t)dataaddr & 0xFF);

    serial->write_serial_data_echo_check(SsmProtocol::addHeader(output, tester_id, target_id, false));
    emit LOG_I("Sent: " + SsmProtocol::toHex(SsmProtocol::addHeader(output, tester_id, target_id, false)), true, true);
    delay(100);
    received = serial->read_serial_data(receive_timeout);
    // received = QByteArray::fromHex("80f018f8e0");
    delay(100);
    emit LOG_I("Recd: " + SsmProtocol::toHex(received), true, true);

    return received;
}

/*
 * SSM Block Read (K-Line)
 *
 * @return received response
 */
QByteArray FlashTcuSubaruHitachiM32rKlineOperation::send_sid_a0_block_read(uint32_t dataaddr, uint32_t datalen)
{
    QByteArray output;
    QByteArray received;
    QByteArray msg;

    output.append((uint8_t)0xa0);
    output.append((uint8_t)0x00);
    output.append((uint8_t)(dataaddr >> 16) & 0xFF);
    output.append((uint8_t)(dataaddr >> 8) & 0xFF);
    output.append((uint8_t)dataaddr & 0xFF);
    output.append((uint8_t)datalen & 0xFF);

    serial->write_serial_data_echo_check(SsmProtocol::addHeader(output, tester_id, target_id, false));
    emit LOG_I("Sent: " + SsmProtocol::toHex(SsmProtocol::addHeader(output, tester_id, target_id, false)), true, true);
    delay(100);
    received = serial->read_serial_data(receive_timeout);
    // received = QByteArray::fromHex("80f01861e06000f000a04115ec6000f000a04115f06000f000a04115f46000f000a04115f86000f000a04115fc1fcef0002e7ff000fe00087e6000f000fe00022684ad806060072004fe0003e07e02f0002eef1fce6200f00081c200ff600401407d0e1081ab");
    delay(100);
    emit LOG_I("Recd: " + SsmProtocol::toHex(received), true, true);

    return received;
}

/*
 * Generate tcu hitachi can seed key from received seed bytes
 *
 * @return seed key (4 bytes)
 */
QByteArray FlashTcuSubaruHitachiM32rKlineOperation::generate_can_seed_key(const QByteArray& requested_seed)
{
    QByteArray key;

    const uint16_t keytogenerateindex[] = {
        0xF2CA, 0x2417, 0x21DE, 0x8475,
        0x39AB, 0xF767, 0x6204, 0x6BE0,
        0xBC63, 0x5988, 0x2845, 0x9846,
        0xEB97, 0x99DE, 0xC7DB, 0xEFAE};

    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

    key = SsmProtocol::calculateSeedKey(requested_seed, keytogenerateindex, indextransformation);

    return key;
}

/*
 * Calculate subaru tcu hitachi seed key from received seed bytes
 *
 * @return seed key (4 bytes)
 */
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
