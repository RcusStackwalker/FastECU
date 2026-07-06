#include "flash_ecu_subaru_unisia_jecs_operation.h"
#include "modules/ssm_protocol.h"
#include "serial_port_actions.h"

#include <QElapsedTimer>
#include <QSerialPort>
#include <QTime>

FlashEcuSubaruUnisiaJecsOperation::FlashEcuSubaruUnisiaJecsOperation(
        SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
        QString cmd_type, QWidget *dialog, QObject *parent, PromptFn promptOverride)
    : FlashOperationWorker(dialog, parent, std::move(promptOverride))
    , serial(serial)
    , ecuCalDef(ecuCalDef)
    , cmd_type(cmd_type)
{
}

bool FlashEcuSubaruUnisiaJecsOperation::execute()
{
    int result = STATUS_ERROR;

    mcu_type_string = ecuCalDef->McuType;
    mcu_type_index = 0;

    while (flashdevices[mcu_type_index].name != 0)
    {
        if (flashdevices[mcu_type_index].name == mcu_type_string)
            break;
        mcu_type_index++;
    }

    // Set serial port
    serial->set_is_iso14230_connection(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(false);
    serial->set_is_29_bit_id(false);
    serial->set_serial_port_baudrate("1953");
    serial->set_serial_port_parity(QSerialPort::EvenParity);
    // Open serial port
    serial->open_serial_port();

    if (cmd_type == "read")
    {
        emit externalLoggerMessage("Reading ROM, please wait...");
        emit LOG_I("Reading ROM from Subaru Unisia Jecs using SSM cable", true, true);
        result = read_mem(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
    }

    return result == STATUS_SUCCESS;
}

/*
 * Read memory from Subaru Denso K-Line 32bit ECUs, nisprog kernel
 *
 * @return success
 */
int FlashEcuSubaruUnisiaJecsOperation::read_mem(uint32_t start_addr, uint32_t length)
{
    QElapsedTimer timer;
    QByteArray output;
    QByteArray received;
    QByteArray received_flush;
    QByteArray msg;
    QByteArray mapdata;

    uint32_t addr = start_addr;
    uint32_t willget = length;
    uint32_t len_done = 0;  //total data written to file
    uint32_t cplen = 0;
    uint32_t timeout = 0;

    bool byte_received = false;
    bool bytes_synced = false;

    #define NP10_MAXBLKS    32   //# of blocks to request per loop. Too high might flood us

    emit  LOG_I("Set ECU to read mode", true, true);

    output.append((uint8_t)0x78);
    output.append((uint8_t)0x12);
    output.append((uint8_t)0x34);
    output.append((uint8_t)0x00);
    serial->write_serial_data_echo_check(output);
    delay(500);
    received_flush = serial->read_serial_data(1000);

    timer.start();
    emit progressChanged(0);

    start_addr = 0;
    addr = start_addr;
    length = 0x20;
    willget = length;
    mapdata.clear();
    mapdata.fill('\x00', start_addr + length);

    received.clear();
    emit LOG_I("Start reading, please wait...", true, true);
    while (willget)
    {
        if (stopRequested())
            return STATUS_ERROR;

        uint32_t numblocks = 0;
        float pleft = 0;
        unsigned long chrono;
        unsigned curspeed = 0;
        unsigned tleft = 0;

        numblocks = willget / 32;

        if (numblocks > NP10_MAXBLKS)
            numblocks = NP10_MAXBLKS;

        uint32_t pagesize = numblocks * 32;

        pleft = (float)(addr - start_addr) / (float)length * 100.0f;
        emit progressChanged(pleft);

        output.clear();
        output.append((uint8_t)0x78);
        output.append((uint8_t)((addr >> 8) & 0xFF));
        output.append((uint8_t)(addr & 0xFF));
        output.append((uint8_t)(0x00 & 0xFF));

        emit LOG_D("Write data: " + SsmProtocol::toHex(output), true, true);
        serial->write_serial_data(output);
        delay(45);

        byte_received = false;
        QTime dieTime;
        uint8_t loop_count = 0;

        received.clear();
        dieTime = QTime::currentTime().addMSecs(500);
        while (!byte_received)
        {
            if (stopRequested())
                return STATUS_ERROR;

            if (QTime::currentTime() > dieTime)
            {
                timeout++;
                dieTime = QTime::currentTime().addMSecs(500);
                serial->write_serial_data(output);
            }

            received.append(serial->read_serial_data(5));
            while (received.length() >= 3)
            {
                if (received.at(0) == output.at(1) && received.at(1) == output.at(2))// && received.at(3) == output.at(1) && received.at(4) == output.at(2))
                {
                    byte_received = true;
                    bytes_synced = true;
                    received.remove(0, 2);
                    mapdata.replace(addr, 1, received);
                    received.remove(0, 1);
                }
                else
                {
                    if (bytes_synced)
                        received.remove(0, 3);
                    else
                        received.remove(0, 1);
                }
                loop_count++;
            }
            if (loop_count > 10)
                bytes_synced = false;
        }
        received_flush = serial->read_serial_data(5);

        cplen = 1;

        chrono = timer.elapsed();

        if (cplen > 0 && chrono > 0)
            curspeed = cplen * (1000.0f / chrono);

        if (!curspeed) {
            curspeed += 1;
        }

        tleft = (willget / curspeed) % 9999;
        tleft++;

        timer.start();

        QString start_address = QString("%1").arg(addr,8,16,QLatin1Char('0')).toUpper();
        QString block_len = QString("%1").arg(pagesize,8,16,QLatin1Char('0')).toUpper();
        msg = QString("Kernel read addr:  0x%1  length:  0x%2,  %3  B/s  %4 s").arg(start_address).arg(block_len).arg(curspeed, 6, 10, QLatin1Char(' ')).arg(tleft, 6, 10, QLatin1Char(' ')).toUtf8();
        emit LOG_I(msg, true, true);
        delay(1);

        addr++;
        willget--;
    }
    emit LOG_I("Reading finished.", true, true);
    qDebug() << "Timeouts:" << timeout;
    qDebug() << "Mapdata length:" << mapdata.length();
    ecuCalDef->FullRomData = mapdata;
    emit progressChanged(100);

    return STATUS_SUCCESS;
}

/*
 * Parse QByteArray to readable form
 *
 * @return parsed message
 */
