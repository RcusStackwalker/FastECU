#include "src/platform/desktop/unix/j2534/driver/J2534_unix.h"

#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <string>

J2534::J2534()
    : rx_buffer_(
          [this]
          {
              if (!is_serial_port_open())
              {
                  return QByteArray{};
              }
              const qint64 available = serial_->bytesAvailable();
              return available > 0 ? serial_->read(available) : QByteArray{};
          },
          [this](int ms)
          {
              if (is_serial_port_open())
              {
                  serial_->waitForReadyRead(ms);
              }
              else
              {
                  // No port to wait on: still yield for `ms` so a closed-port
                  // read costs the same wall-clock as before (SerialByteBuffer's
                  // take() loop calls this every iteration until its own
                  // deadline), instead of busy-spinning at 100% CPU for the
                  // whole timeout with no actual wait happening.
                  delay(ms);
              }
          },
          []
          {
              return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now().time_since_epoch())
                                                    .count());
          })
{
}

J2534::~J2534()
{
}

QString J2534::open_serial_port(const QString& serial_port)
{
    if (opened_serial_port != serial_port)
    {
        if (!(serial_->isOpen() && serial_->isWritable()))
        {
            serial_->setPortName(serial_port);
            serial_->setBaudRate(static_cast<qint32>(serial_port_baudrate.toDouble()));
            serial_->setDataBits(QSerialPort::Data8);
            serial_->setStopBits(QSerialPort::OneStop);
            serial_->setParity(QSerialPort::NoParity);
            serial_->setFlowControl(QSerialPort::NoFlowControl);

            if (serial_->open(QIODevice::ReadWrite))
            {
                // serial->setDataTerminalReady(setDataTerminalReady);
                // serial->setRequestToSend(setRequestToSend);
                serial_->clearError();
                serial_->clear();
                serial_->flush();
                rx_buffer_.clear();
                opened_serial_port = serial_port;
                // connect(serial, SIGNAL(readyRead()), this, SLOT(ReadSerialDataSlot()), Qt::DirectConnection);
                qRegisterMetaType<QSerialPort::SerialPortError>();
                connect(serial_, SIGNAL(errorOccurred(QSerialPort::SerialPortError)), this,
                        SLOT(handle_error(QSerialPort::SerialPortError)));

                emit LOG_D("Linux j2534 serial port '" + serial_port + "' is open at baudrate " + serial_port_baudrate,
                           true, true);
                return opened_serial_port;
            }
            else
            {
                emit LOG_D("Couldn't open Linux j2534 serial port '" + serial_port + "'", true, true);
                return {};
            }
        }
        else
        {
            emit LOG_D("Linux j2534 serial port '" + serial_port + "' is already opened", true, true);
            return opened_serial_port;
        }
    }

    return opened_serial_port;
}

void J2534::close_serial_port()
{
    if (is_serial_port_open())
    {
        serial_->close();
        rx_buffer_.clear();
        delay(100);
    }
    opened_serial_port = "";
}

bool J2534::is_serial_port_open()
{
    // Guard the pointer: the serial port can be torn down (reset/reconnect)
    // while a read is in flight. Dereferencing a null `serial` here is the
    // crash in the field report (EXC_BAD_ACCESS at 0x8 in QIODevice::isOpen()).
    return serial_ && serial_->isOpen();
}

QByteArray J2534::read_serial_data(uint32_t datalen, uint16_t timeout)
{
    return rx_buffer_.take(datalen, timeout);
}

int J2534::write_serial_data(const QByteArray& output)
{
    if (!is_serial_port_open())
    {
        return 1;
    }
    // One write, then an explicit flush. The previous per-byte loop left the
    // data sitting in Qt's write buffer until some later waitForReadyRead
    // happened to pump the event loop, so transmission was both fragmented
    // and timed by an unrelated call.
    if (serial_->write(output) != output.size())
    {
        return 1;
    }
    if (!serial_->waitForBytesWritten(serial_read_short_timeout))
    {
        return 1;
    }
    return kJ2534StatusNoerror;
}

QByteArray J2534::write_serial_iso14230_data(QByteArray output)
{
    uint8_t chk_sum = 0;
    uint8_t msglength = output.length();

    output.insert(0, static_cast<char>(0x80));
    output.insert(1, 0x10);
    output.insert(2, static_cast<char>(0xFC));
    output.insert(3, static_cast<char>(msglength));

    for (int i = 0; i < output.length(); i++)
    {
        chk_sum = chk_sum + output.at(i);
    }
    output.append(static_cast<char>(chk_sum));

    return output;
}

bool J2534::get_is_tx_done()
{
    return is_tx_done;
}

QString J2534::parseMessageToHex(const QByteArray& received)
{
    QByteArray msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}

uint32_t J2534::parse_ts(const char *data)
{
    uint32_t timestamp = 0;
    memcpy(&timestamp, data, 4);
    // if (littleEndian)
    //     timestamp = bswap(timestamp);
    return timestamp;
}

long J2534::PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    QByteArray output;
    QByteArray received;
    QByteArray check_result = "ar";
    QString name = (char *)pName;
    unsigned long *devID = (unsigned long *)pDeviceID;
    long result = kJ2534ErrNotSupported;

    pDeviceID = 0;
    emit LOG_D("Open J2534 device " + name + " with ID: " + QString::number(*devID), true, true);

    output = "ata\r\n";
    emit LOG_D("Send data: " + parseMessageToHex(output), true, true);
    write_serial_data(output);
    received = read_serial_data(7, 50);
    emit LOG_D("Result check against " + check_result + ": " + parseMessageToHex(received), true, true);
    if (received.startsWith(check_result))
    {
        emit LOG_D("Result check OK", true, true);
        result = kJ2534StatusNoerror;
    }
    else
    {
        emit LOG_D("Result check failed, not maybe an j2534 interface!", true, true);
    }

    return result;
}

long J2534::PassThruClose(unsigned long DeviceID)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    // emit LOG_D("Close J2534 device ID:" << DeviceID;

    output = "atz\r\n";
    // emit LOG_D("Send data:" << output;
    write_serial_data(output);
    received = read_serial_data(8, 50);
    // emit LOG_D("Received:" << received;
    close_serial_port();

    return result;
}

long J2534::PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                            unsigned long Baudrate, unsigned long *pChannelID)
{
    QByteArray output;
    QByteArray received;
    unsigned long chanID = (unsigned long)pChannelID;
    long result = kJ2534StatusNoerror;

    switch ((int)ProtocolID)
    {
    case kJ2534Iso9141:
    case kJ2534Iso14230:
    case kJ2534Can:
    case kJ2534Iso15765:
    case kJ2534CanCh1:
        break;
    default:
        return 0; // J2534_ERR_INVALID_PROTOCOL_ID;
    }

    output.clear();
    QString str = "ato" + QString::number(ProtocolID) + " " + QString::number(Flags) + " " + QString::number(Baudrate) +
                  " " + QString::number(ProtocolID) + "\r\n";
    output.append(str.toUtf8());
    // emit LOG_D("Send data:" << output;
    write_serial_data(output);
    received = read_serial_data(100, 50);
    emit LOG_D("Connect received: " + parseMessageToHex(received) + " " + received + " " + QString::number(chanID),
               true, true);

    return result;
}

long J2534::PassThruDisconnect(unsigned long ChannelID)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    // emit LOG_D("Disconnect J2534 device in channel:" << ChannelID;

    output.clear();
    QString str = "atc" + QString::number(ChannelID) + "\r\n";
    output.append(str.toUtf8());
    // emit LOG_D("Send data:" << output;
    write_serial_data(output);
    received = read_serial_data(100, 50);
    // emit LOG_D("Received:" << received;

    return result;
}

long J2534::PassThruReadMsgs(unsigned long ChannelID, PassThruMsg *pMsg, unsigned long *pNumMsgs, unsigned long Timeout)
{
    QByteArray received;
    QByteArray msg;
    long result = kJ2534StatusNoerror;
    unsigned long msg_cnt = 0;
    unsigned long chunk_cnt = 0;
    uint8_t msg_type = 0;
    uint32_t msg_index = 0;
    QString msg_type_string = 0;
    unsigned long msg_byte_cnt = 0;
    bool stop_reading = false;

    received = read_serial_data(3, Timeout);
    // emit LOG_D("Recieved data: " + parseMessageToHex(received), true, true);
    while (received.length() > 0 && is_serial_port_open())
    {
        // emit LOG_D("Message header: " + parseMessageToHex(received), true, true);
        if (received.at(0) == 0x61 && received.at(1) == 0x72)
        {
            if (received.at(2) == 'o') // ACK 0x6f
            {
                read_serial_data(2, Timeout);
                // emit LOG_D("Sent msg ACK: " + parseMessageToHex(received), true, true);
                received.clear();
                msg_ack = true;
            }
            else if (received.at(2) == 'e') // error 0x65
            {
                while ((uint8_t)received.at(received.length() - 1) == 0x0d)
                {
                    received.append(read_serial_data(1, Timeout));
                }
                // emit LOG_D("Error sending message: " + parseMessageToHex(received), true, true);
                received.clear();
            }
            else if (received.at(2) == 'm') // 0x6d
            {
                received.append(read_serial_data(2, Timeout));
                msg.clear();
                while ((uint8_t)msg[msg.length() - 1] != 0x20)
                {
                    msg.append(read_serial_data(1, Timeout));
                }
                received.append(msg);
                periodic_msg_id = msg.remove(msg.length() - 1, 1).toULong();

                while ((uint8_t)msg[msg.length() - 1] != 0x0a)
                {
                    msg.append(read_serial_data(1, Timeout));
                }
                received.append(msg);
                msg = read_serial_data(msg_byte_cnt, Timeout);
                received.append(msg);

                msg_index = 0;
                // emit LOG_D("Periodic msg response: " + parseMessageToHex(received), true, true);
            }
            else if (received.at(2) == 'r') // 0x72
            {
                while ((uint8_t)received.at(received.length() - 1) != 0x0a)
                {
                    received.append(read_serial_data(1, Timeout));
                }
                for (int i = 0; i < received.length(); i++)
                {
                    pMsg->data[i] = (uint8_t)received.at(i);
                }
                pMsg->data_size = received.length();

                // emit LOG_D("vBatt msg response: " + parseMessageToHex(received), true, true);
                return kJ2534StatusNoerror;
            }
            else if (received.at(2) == 'w') // 0x77
            {
                while ((uint8_t)received.at(received.length() - 1) != 0x0a)
                {
                    received.append(read_serial_data(1, Timeout));
                }
                for (int i = 0; i < received.length(); i++)
                {
                    pMsg->data[i] = (uint8_t)received.at(i);
                }
                pMsg->data_size = received.length();

                // emit LOG_D("Five baud init msg response: " + parseMessageToHex(received), true, true);
                return kJ2534StatusNoerror;
            }
            else if (received.at(2) == 'y') // 0x79
            {
                received.append(read_serial_data(2, Timeout));
                msg.clear();
                while ((uint8_t)msg[msg.length() - 1] != 0x20)
                {
                    msg.append(read_serial_data(1, Timeout));
                }
                received.append(msg);
                msg_byte_cnt = msg.remove(msg.length() - 1, 1).toInt();

                while ((uint8_t)msg[msg.length() - 1] != 0x0a)
                {
                    msg.append(read_serial_data(1, Timeout));
                }
                received.append(msg);
                msg = read_serial_data(msg_byte_cnt, Timeout);
                received.append(msg);

                msg_index = 0;
                for (unsigned long i = 0; i < msg_byte_cnt; i++)
                {
                    pMsg->data[msg_index++] = (uint8_t)msg.at(static_cast<qsizetype>(i));
                }

                pMsg->rx_status = NORM_MSG;
                pMsg->data_size = msg_index;
                msg_cnt++;
                // emit LOG_D("Fast init msg response: " + parseMessageToHex(received), true, true);
            }
            else if (received.at(2) == '3' || received.at(2) == '4' || received.at(2) == '5' || received.at(2) == '6')
            {
                received.append(read_serial_data(2, Timeout));
                msg_byte_cnt = received.at(3) - 1;
                msg_type = received.at(4);
                switch (msg_type)
                {
                case NORM_MSG:
                    msg_type_string = "NORM_MSG";
                    break;
                case kJ2534StartOfMessage:
                    msg_type_string = "START_OF_MESSAGE";
                    break;
                case TX_DONE_MSG:
                    msg_type_string = "TX_DONE_MSG";
                    break;
                case TX_LB_MSG:
                    msg_type_string = "TX_LB_MSG";
                    break;
                case RX_MSG_END_IND:
                    msg_type_string = "RX_MSG_END_IND";
                    break;
                case EXT_ADDR_MSG_END_IND:
                    msg_type_string = "EXT_ADDR_MSG_END_IND";
                    break;
                case LB_MSG_END_IND:
                    msg_type_string = "LB_MSG_END_IND";
                    break;
                case NORM_MSG_START_IND:
                    msg_type_string = "NORM_MSG_START_IND";
                    break;
                case TX_LB_START_IND:
                    msg_type_string = "TX_LB_START_IND";
                    break;
                default:
                    // emit LOG_D("HEADER" << parseMessageToHex(received);
                    break;
                }
                // emit LOG_D("Message received at channel: " + QString::number(ChannelID) + ", size is: " +
                // QString::number(msg_byte_cnt) + " and message type: " + msg_type_string + " (0x" +
                // QString::number(msg_type, 16) + ")", true, true);

                if (msg_type == kJ2534StartOfMessage)
                {
                    msg_index = 0;
                    msg_cnt++;
                    // emit LOG_D("START_OF_MESSAGE: " + parseMessageToHex(received), true, true);
                }

                if (msg_type == TX_DONE_MSG)
                {
                    pMsg->rx_status = TX_DONE_MSG;
                    received.append(read_serial_data(msg_byte_cnt, Timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("TX_DONE_MSG: " + parseMessageToHex(received), true, true);
                    received.clear();
                    is_tx_done = true;
                }
                if (msg_type == TX_LB_START_IND)
                {
                    pMsg->rx_status = TX_LB_START_IND;
                    received.append(read_serial_data(msg_byte_cnt, Timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("TX_LB_START_IND: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == TX_LB_MSG)
                {
                    pMsg->rx_status = TX_LB_MSG;
                    received.append(read_serial_data(msg_byte_cnt, Timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("TX_LB_MSG: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == LB_MSG_END_IND)
                {
                    pMsg->rx_status = LB_MSG_END_IND;
                    received.append(read_serial_data(msg_byte_cnt, Timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("LB_MSG_END_IND: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == NORM_MSG_START_IND)
                {
                    pMsg->rx_status = NORM_MSG_START_IND;
                    received.append(read_serial_data(msg_byte_cnt, Timeout));

                    msg_index = 0;
                    msg_cnt++;
                    chunk_cnt = 0;

                    // emit LOG_D("NORM_MSG_START_IND: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == NORM_MSG || msg_type == kJ2534StartOfMessage)
                {
                    pMsg->rx_status = NORM_MSG;

                    received.append(read_serial_data(msg_byte_cnt, Timeout));
                    // emit LOG_D("NORM_MSG: " + parseMessageToHex(received), true, true);

                    if (received.at(2) == '5' || received.at(2) == '6')
                    {
                        msg_byte_cnt -= 4;
                        if (chunk_cnt)
                        {
                            msg_byte_cnt -= 4;
                        }
                    }
                    // emit LOG_D("Message byte count: " + QString::number(msg_byte_cnt), true, true);
                    for (unsigned long i = 0; i < msg_byte_cnt; i++)
                    {
                        if (received.at(2) == '3' || received.at(2) == '4')
                        {
                            pMsg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 5);
                        }
                        if (received.at(2) == '5' || received.at(2) == '6')
                        {
                            if (chunk_cnt)
                            {
                                pMsg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 13);
                            }
                            else
                            {
                                pMsg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 9);
                            }
                        }
                    }
                    chunk_cnt++;

                    if (received.at(2) == '5')
                    {
                        std::array<char, 4> data{};
                        data[0] = received.at(8);
                        data[1] = received.at(7);
                        data[2] = received.at(6);
                        data[3] = received.at(5);
                        pMsg->timestamp = parse_ts(data.data());
                        pMsg->data_size = msg_index;
                        msg_cnt++;
                        stop_reading = true;
                    }
                    received.clear();
                }
                if (msg_type == RX_MSG_END_IND)
                {
                    pMsg->rx_status = RX_MSG_END_IND;

                    received.append(read_serial_data(msg_byte_cnt, Timeout));

                    if (received.at(2) == '6')
                    {
                        msg_byte_cnt -= 4;
                        if (chunk_cnt)
                        {
                            msg_byte_cnt -= 4;
                        }
                        for (unsigned long i = 0; i < msg_byte_cnt; i++)
                        {
                            if (chunk_cnt)
                            {
                                pMsg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 13);
                            }
                            else
                            {
                                pMsg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 9);
                            }
                        }
                    }
                    std::array<char, 4> data{};
                    data[0] = received.at(8);
                    data[1] = received.at(7);
                    data[2] = received.at(6);
                    data[3] = received.at(5);
                    pMsg->timestamp = parse_ts(data.data());
                    pMsg->data_size = msg_index;
                    msg_cnt++;

                    received.clear();
                    stop_reading = true;
                }
            }
        }
        if (!stop_reading)
        {
            QByteArray response = read_serial_data(3, Timeout);
            if (response.length() > 0)
            {
                // emit LOG_D("Added response: " + parseMessageToHex(response), true, true);
                received.append(response);
            }
            else
            {
                received.clear();
            }
        }
        // emit LOG_D("Parsing read messages:" << received.length() << received << parseMessageToHex(received);
    }

    *pNumMsgs = msg_cnt;
    received.clear();
    return result;
}

long J2534::PassThruWriteMsgs(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pNumMsgs,
                              unsigned long Timeout)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    for (unsigned long msg_index = 0; msg_index < *pNumMsgs; msg_index++)
    {
        output.clear();
        QString str = "att" + QString::number(ChannelID) + " " + QString::number(pMsg->data_size) + " " +
                      QString::number(pMsg->tx_flags) + "\r\n";
        output.append(str.toUtf8());
        for (unsigned long i = 0; i < pMsg->data_size; i++)
        {
            output.append(static_cast<char>(pMsg->data[i]));
        }
        write_serial_data(output);
        pMsg++;
        is_tx_done = false;
    }

    return result;
}

long J2534::PassThruStartPeriodicMsg(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pMsgID,
                                     unsigned long TimeInterval)
{
    QByteArray output;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atm" + QString::number(ChannelID) + " " + QString::number(TimeInterval * 1000) + " 0 " +
                  QString::number(pMsg->tx_flags) + " " + QString::number(pMsg->data_size) + "\r\n";
    output.append(str.toUtf8());
    for (unsigned long i = 0; i < pMsg->data_size; i++)
    {
        output.append(static_cast<char>(pMsg->data[i]));
    }

    write_serial_data(output);
    // PassThruReadMsgs(ChannelID, &rxmsg, &numRxMsg, timeout);

    *pMsgID = periodic_msg_id;

    return result;
}

long J2534::PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    QByteArray output;
    long result = kJ2534StatusNoerror;

    QString str = "atn" + QString::number(ChannelID) + " " + QString::number(MsgID) + "\r\n";
    output.append(str.toUtf8());

    write_serial_data(output);

    return result;
}

long J2534::PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PassThruMsg *pMaskMsg,
                                   const PassThruMsg *pPatternMsg, const PassThruMsg *pFlowControlMsg,
                                   unsigned long *pMsgID)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atf" + QString::number(ChannelID) + " " + QString::number(FilterType) + " " +
                  QString::number(pMaskMsg->tx_flags) + " " + QString::number(pMaskMsg->data_size);
    output.append(str.toUtf8());
    output.append("\r\n");

    for (unsigned long i = 0; i < pMaskMsg->data_size; i++)
    {
        output.append(static_cast<char>(pMaskMsg->data[i]));
    }
    for (unsigned long i = 0; i < pPatternMsg->data_size; i++)
    {
        output.append(static_cast<char>(pPatternMsg->data[i]));
    }
    if (pFlowControlMsg)
    {
        for (unsigned long i = 0; i < pFlowControlMsg->data_size; i++)
        {
            output.append(static_cast<char>(pFlowControlMsg->data[i]));
        }
    }
    // emit LOG_D("Send data:" << parseMessageToHex(output);
    write_serial_data(output);
    received = read_serial_data(100, 50);
    // emit LOG_D("Received:" << received;

    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID)
{
    long result = kJ2534StatusNoerror;

    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atv" + QString::number(Pin) + " " + QString::number(Voltage) + "\r\n";
    output.append(str.toUtf8());
    write_serial_data(output);
    // received = read_serial_data(5, 50);

    return result;
}

long J2534::PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    const std::size_t apiLen = std::min(kApiVersion.size(), kVersionBufferSize - 1);
    std::memcpy(pApiVersion, kApiVersion.data(), apiLen);
    pApiVersion[apiLen] = '\0';
    const std::size_t dllLen = std::min(kDllVersion.size(), kVersionBufferSize - 1);
    std::memcpy(pDllVersion, kDllVersion.data(), dllLen);
    pDllVersion[dllLen] = '\0';
    // strncpy(pFirmwareVersion, fw_version, strlen(fw_version));

    output = "\r\n\r\nati\r\n";
    emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
    write_serial_data(output);
    delay(50);
    received = read_serial_data(50, 100);
    emit LOG_D("Response: " + parseMessageToHex(received), true, true);
    QString response = QString::fromUtf8(received);
    QStringList fw_ver = response.split("ari ");
    fw_ver = fw_ver.at(fw_ver.length() - 1).split("\r\n");
    // Hold the bytes in a named QByteArray: `fw_ver.at(0).toUtf8().data()` would
    // dangle (the temporary QByteArray is destroyed at the semicolon), leaving
    // pFirmwareVersion empty or holding garbage.
    const QByteArray fw_ver_bytes = fw_ver.at(0).toUtf8();
    const std::size_t fwLen = std::min<std::size_t>(fw_ver_bytes.size(), kVersionBufferSize - 1);
    std::memcpy(pFirmwareVersion, fw_ver_bytes.constData(), fwLen);
    pFirmwareVersion[fwLen] = '\0';

    return result;
}

long J2534::PassThruGetLastError(char *pErrorDescription)
{
    long result = kJ2534StatusNoerror;

    return result;
}

int J2534::is_valid_sconfig_param(SCONFIG s)
{
    switch (s.parameter)
    {
    case kJ2534P1Min:
    case kJ2534P2Min:
    case kJ2534P3Max:
    case kJ2534P4Max:
        return 0;
    default:
        return 1;
    }
}

void J2534::dump_sbyte_array(const SByteArray *s)
{
    // emit LOG_D("SByteArray size =" << s->NumOfBytes;
    // DBGPRINT(("SByteArray size=%u\n",s->NumOfBytes));
    // DBGDUMP((s->BytePtr,s->NumOfBytes,0));
}

void J2534::dump_sconfig_param(SCONFIG s)
{
    std::string paramName;

    switch (s.parameter)
    {
    case kJ2534DataRate:
        paramName = "DATA_RATE";
        break;
    case kJ2534Loopback:
        paramName = "LOOPBACK";
        break;
    case kJ2534NodeAddress:
        paramName = "NODE_ADDRESS";
        break;
    case kJ2534NetworkLine:
        paramName = "NETWORK_LINE";
        break;
    case kJ2534P1Min:
        paramName = "P1_MIN";
        break;
    case kJ2534P1Max:
        paramName = "P1_MAX";
        break;
    case kJ2534P2Min:
        paramName = "P2_MIN";
        break;
    case kJ2534P2Max:
        paramName = "P2_MAX";
        break;
    case kJ2534P3Min:
        paramName = "P3_MIN";
        break;
    case kJ2534P3Max:
        paramName = "P3_MAX";
        break;
    case kJ2534P4Min:
        paramName = "P4_MIN";
        break;
    case kJ2534P4Max:
        paramName = "P4_MAX";
        break;
    case kJ2534W1:
        paramName = "W1";
        break;
    case kJ2534W2:
        paramName = "W2";
        break;
    case kJ2534W3:
        paramName = "W3";
        break;
    case kJ2534W4:
        paramName = "W4";
        break;
    case kJ2534W5:
        paramName = "W5";
        break;
    case kJ2534Tidle:
        paramName = "TIDLE";
        break;
    case kJ2534Tinil:
        paramName = "TINIL";
        break;
    case kJ2534Twup:
        paramName = "TWUP";
        break;
    case kJ2534Parity:
        paramName = "PARITY";
        break;
    case kJ2534BitSamplePoint:
        paramName = "BIT_SAMPLE_POINT";
        break;
    case kJ2534SyncJumpWidth:
        paramName = "SYNC_JUMP_WIDTH";
        break;
    case kJ2534W0:
        paramName = "W0";
        break;
    case kJ2534T1Max:
        paramName = "T1_MAX";
        break;
    case kJ2534T2Max:
        paramName = "T2_MAX";
        break;
    case kJ2534T4Max:
        paramName = "T4_MAX";
        break;
    case kJ2534T5Max:
        paramName = "T5_MAX";
        break;
    case kJ2534Iso15765Bs:
        paramName = "ISO15765_BS";
        break;
    case kJ2534Iso15765Stmin:
        paramName = "ISO15765_STMIN";
        break;
    case kJ2534DataBits:
        paramName = "DATA_BITS";
        break;
    case kJ2534FiveBaudMod:
        paramName = "FIVE_BAUD_MOD";
        break;
    case kJ2534BsTx:
        paramName = "BS_TX";
        break;
    case kJ2534StminTx:
        paramName = "STMIN_TX";
        break;
    case kJ2534T3Max:
        paramName = "T3_MAX";
        break;
    case kJ2534Iso15765WftMax:
        paramName = "ISO15765_WFT_MAX";
        break;
    case kJ2534CanMixedFormat:
        paramName = "CAN_MIXED_FORMAT";
        break;
    case kJ2534J1962Pins:
        paramName = "J1962_PINS";
        break;
    case kJ2534SwCanHsDataRate:
        paramName = "W_CAN_HS_DATA_RATE";
        break;
    case kJ2534SwCanSpeedchangeEnable:
        paramName = "SW_CAN_SPEEDCHANGE_ENABLE";
        break;
    case kJ2534SwCanResSwitch:
        paramName = "SW_CAN_RES_SWITCH";
        break;
    case kJ2534ActiveChannels:
        paramName = "ACTIVE_CHANNELS";
        break;
    case kJ2534SampleRate:
        paramName = "SAMPLE_RATE";
        break;
    case kJ2534SamplesPerReading:
        paramName = "SAMPLES_PER_READING";
        break;
    case kJ2534ReadingsPerMsg:
        paramName = "READINGS_PER_MSG";
        break;
    case kJ2534AveragingMethod:
        paramName = "AVERAGING_METHOD";
        break;
    case kJ2534SampleResolution:
        paramName = "SAMPLE_RESOLUTION";
        break;
    case kJ2534InputRangeLow:
        paramName = "INPUT_RANGE_LOW";
        break;
    case kJ2534InputRangeHigh:
        paramName = "INPUT_RANGE_HIGH";
        break;
    default:
        paramName = std::format("{}(unknown)", s.parameter);
        break;
    }

    // DBGPRINT(("    %s : %u",paramName,s.Value));
    // emit LOG_D("    " << paramName << s.Value;
}

long J2534::PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput)
{
    QByteArray output;
    QByteArray received;
    uint32_t par_cnt = 0;

    int input_as_sa = 0;
    int output_as_sa = 0;
    unsigned int i;
    SConfigList *scl;
    long result = kJ2534StatusNoerror;
    std::string IoctlName;

    // const SConfigList *inputlist = pInput;

    switch (IoctlID)
    {
    case kJ2534GetConfig:
        IoctlName = "GET_CONFIG";
        break;
    case kJ2534SetConfig:
        IoctlName = "SET_CONFIG";
        break;
    case kJ2534ReadVbatt:
        IoctlName = "READ_VBATT";
        break;
    case kJ2534FiveBaudInit:
        IoctlName = "FIVE_BAUD_INIT";
        input_as_sa = 1;
        output_as_sa = 1;
        break;
    case kJ2534FastInit:
        IoctlName = "FAST_INIT";
        break;
    case kJ2534ClearTxBuffer:
        IoctlName = "CLEAR_TX_BUFFER";
        break;
    case kJ2534ClearRxBuffer:
        IoctlName = "CLEAR_RX_BUFFER";
        break;
    case kJ2534ClearPeriodicMsgs:
        IoctlName = "CLEAR_PERIODIC_MSGS";
        break;
    case kJ2534ClearMsgFilters:
        IoctlName = "CLEAR_MSG_FILTERS";
        break;
    case kJ2534ClearFunctMsgLookupTable:
        IoctlName = "CLEAR_FUNCT_MSG_LOOKUP_TABLE";
        break;
    case kJ2534AddToFunctMsgLookupTable:
        IoctlName = "ADD_TO_FUNCT_MSG_LOOKUP_TABLE";
        break;
    case kJ2534DeleteFromFunctMsgLookupTable:
        IoctlName = "DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE";
        break;
    case kJ2534ReadProgVoltage:
        IoctlName = "READ_PROG_VOLTAGE";
        break;
        //    case TX_IOCTL_APP_SERVICE:
        //        strcpy(IoctlName,"APP_SERVICE");
        //        break;
    default:
        IoctlName = std::format("{}(unknown)", IoctlID);
        break;
    }

    if (IoctlID == kJ2534GetConfig)
    {
    }
    if (IoctlID == kJ2534SetConfig)
    {
        pOutput = nullptr; // make some DLLs happy

        // dump params
        scl = (SConfigList *)pInput;
        for (i = 0; i < scl->num_of_params; i++)
        {
            dump_sconfig_param((scl->config_ptr)[i]);
        }

        // Enabling this could break some J2534 devices such as Denso DST-i
        /*for (i = 0; i < scl->NumOfParams; i++)
            if (!is_valid_sconfig_param((scl->ConfigPtr)[i]))
            {
                //emit LOG_D("param not allowed - not passing through and instead faking success" << result;
                return STATUS_NOERROR;
            }*/

        SCONFIG *cfgitem_local;
        par_cnt = scl->num_of_params;
        for (i = 0; i < par_cnt; ++i)
        {
            cfgitem_local = &scl->config_ptr[i];
            output.clear();
            QString str = "ats" + QString::number(ChannelID) + " " + QString::number(cfgitem_local->parameter) + " " +
                          QString::number(cfgitem_local->value) + "\r\n";
            output.append(str.toUtf8());
            write_serial_data(output);
            emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
            received = read_serial_data(100, 50);
            emit LOG_D("Response: " + parseMessageToHex(received), true, true);
        }
    }
    if (IoctlID == kJ2534ReadVbatt)
    {
        PassThruMsg rxmsg;
        unsigned long numRxMsg;
        QByteArray received_local;
        rxmsg.data_size = 0;
        numRxMsg = 1;

        unsigned long *vBatt = (unsigned long *)pOutput;
        long pin = 16;
        output.clear();
        QString str = "atr " + QString::number((int)pin) + "\r\n";
        output.append(str.toUtf8());
        if (!is_serial_port_open())
        {
            return result;
        }
        if (serial_->bytesAvailable())
        {
            return result;
        }
        write_serial_data(output);
        emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
        result = PassThruReadMsgs(ChannelID, &rxmsg, &numRxMsg, serial_read_timeout);
        if (result)
        {
            return result;
        }
        received_local.clear();
        for (unsigned long i_local = 0; i_local < rxmsg.data_size; i_local++)
        {
            received_local.append(static_cast<char>(rxmsg.data[i_local]));
        }
        emit LOG_D("Response: " + parseMessageToHex(received_local), true, true);
        QString response = QString(received_local).split(" ").at(QString(received_local).split(" ").length() - 1);
        response = response.split("\r\n").at(0);
        // emit LOG_D("Pin 16 voltage: " + response + " mV", true, true);
        *vBatt = response.toULong();
    }

    if (IoctlID == kJ2534FiveBaudInit)
    {
        PassThruMsg rxmsg;
        unsigned long numRxMsg;
        QByteArray received_local;
        rxmsg.data_size = 0;
        numRxMsg = 1;
        SByteArray *msg = (SByteArray *)pInput;
        SByteArray *response = (SByteArray *)pOutput;

        output.clear();
        QString str = "atw" + QString::number(ChannelID) + " " + QString::number(msg->byte_ptr[0]) + " 0\r\n";
        output.append(str.toUtf8());
        write_serial_data(output);
        emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
        memset(&rxmsg, 0, sizeof(rxmsg));
        result = PassThruReadMsgs(ChannelID, &rxmsg, &numRxMsg, serial_read_extra_long_timeout);
        if (result)
        {
            return result;
        }
        received_local.clear();
        for (unsigned long i_local = 0; i_local < rxmsg.data_size; i_local++)
        {
            response->byte_ptr[i_local] = rxmsg.data[i_local];
            received_local.append(static_cast<char>(rxmsg.data[i_local]));
        }
        response->num_of_bytes = rxmsg.data_size;
        emit LOG_D("Response: " + parseMessageToHex(received_local), true, true);
    }

    if (IoctlID == kJ2534FastInit)
    {
        PassThruMsg *msg = (PassThruMsg *)pInput;

        output.clear();
        QString str = "aty" + QString::number(ChannelID) + " " + QString::number(msg->data_size) + " 0\r\n";
        output.append(str.toUtf8());
        for (i = 0; i < msg->data_size; i++)
        {
            output.append(static_cast<char>(msg->data[i]));
        }
        write_serial_data(output);
        emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
    }

    if (input_as_sa)
    {
        // emit LOG_D("Input", true, true);
        dump_sbyte_array((SByteArray *)pInput);
    }

    // result = (*pfPassThruIoctl)(ChannelID,IoctlID,pInput,pOutput);

    if (output_as_sa)
    {
        // emit LOG_D("Output", true, true);
        dump_sbyte_array((SByteArray *)pOutput);
    }

    return result;
}

void J2534::delay(int n)
{
    QThread::msleep(n);
}

void J2534::handle_error(QSerialPort::SerialPortError error)
{
    // emit LOG_D("Error:" << QString::number(error), true, true);

    switch (error)
    {
    case QSerialPort::DeviceNotFoundError:
    case QSerialPort::OpenError:
    case QSerialPort::NotOpenError:
    case QSerialPort::WriteError:
    case QSerialPort::ReadError:
    case QSerialPort::ResourceError:
    case QSerialPort::UnknownError:
        close_serial_port();
        break;
    case QSerialPort::TimeoutError:
        // read_serial_data now polls via serial->waitForReadyRead(1) instead of
        // pumping the event loop; QSerialPort emits this error whenever such a
        // wait elapses with no new bytes, which is the ordinary case on every
        // polling tick, not a device/link failure. Previously this error was
        // unreachable (nothing called a wait-with-timeout API), so it could
        // safely close the port; now that would tear down the connection on
        // routine polling latency. Leave it a no-op, like the other
        // non-fatal errors.
    // NoError, PermissionError, UnsupportedOperationError and the Parity/Framing/BreakCondition
    // errors are non-fatal and deliberately left alone.
    default:
        break;
    }
}
