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
              if (!IsSerialPortOpen())
              {
                  return QByteArray{};
              }
              const qint64 available = serial_->bytesAvailable();
              return available > 0 ? serial_->read(available) : QByteArray{};
          },
          [this](int ms)
          {
              if (IsSerialPortOpen())
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
                  Delay(ms);
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

QString J2534::OpenSerialPort(const QString& serial_port)
{
    if (opened_serial_port_ != serial_port)
    {
        if (!(serial_->isOpen() && serial_->isWritable()))
        {
            serial_->setPortName(serial_port);
            serial_->setBaudRate(static_cast<qint32>(serial_port_baudrate_.toDouble()));
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
                rx_buffer_.Clear();
                opened_serial_port_ = serial_port;
                // connect(serial, SIGNAL(readyRead()), this, SLOT(ReadSerialDataSlot()), Qt::DirectConnection);
                qRegisterMetaType<QSerialPort::SerialPortError>();
                connect(serial_, SIGNAL(errorOccurred(QSerialPort::SerialPortError)), this,
                        SLOT(handleError(QSerialPort::SerialPortError)));

                emit logD("Linux j2534 serial port '" + serial_port + "' is open at baudrate " + serial_port_baudrate_,
                          true, true);
                return opened_serial_port_;
            }
            else
            {
                emit logD("Couldn't open Linux j2534 serial port '" + serial_port + "'", true, true);
                return {};
            }
        }
        else
        {
            emit logD("Linux j2534 serial port '" + serial_port + "' is already opened", true, true);
            return opened_serial_port_;
        }
    }

    return opened_serial_port_;
}

void J2534::CloseSerialPort()
{
    if (IsSerialPortOpen())
    {
        serial_->close();
        rx_buffer_.Clear();
        Delay(100);
    }
    opened_serial_port_ = "";
}

bool J2534::IsSerialPortOpen()
{
    // Guard the pointer: the serial port can be torn down (reset/reconnect)
    // while a read is in flight. Dereferencing a null `serial` here is the
    // crash in the field report (EXC_BAD_ACCESS at 0x8 in QIODevice::isOpen()).
    return serial_ && serial_->isOpen();
}

QByteArray J2534::ReadSerialData(uint32_t datalen, uint16_t timeout)
{
    return rx_buffer_.Take(datalen, timeout);
}

int J2534::WriteSerialData(const QByteArray& output)
{
    if (!IsSerialPortOpen())
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
    if (!serial_->waitForBytesWritten(serial_read_short_timeout_))
    {
        return 1;
    }
    return kJ2534StatusNoerror;
}

QByteArray J2534::WriteSerialIso14230Data(QByteArray output)
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

bool J2534::GetIsTxDone()
{
    return is_tx_done_;
}

QString J2534::ParseMessageToHex(const QByteArray& received)
{
    QByteArray msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}

uint32_t J2534::ParseTs(const char *data)
{
    uint32_t timestamp = 0;
    memcpy(&timestamp, data, 4);
    // if (littleEndian)
    //     timestamp = bswap(timestamp);
    return timestamp;
}

long J2534::PassThruOpen(const void *p_name, unsigned long *p_device_id)
{
    QByteArray output;
    QByteArray received;
    QByteArray check_result = "ar";
    QString name = (char *)p_name;
    unsigned long *dev_id = (unsigned long *)p_device_id;
    long result = kJ2534ErrNotSupported;

    p_device_id = 0;
    emit logD("Open J2534 device " + name + " with ID: " + QString::number(*dev_id), true, true);

    output = "ata\r\n";
    emit logD("Send data: " + ParseMessageToHex(output), true, true);
    WriteSerialData(output);
    received = ReadSerialData(7, 50);
    emit logD("Result check against " + check_result + ": " + ParseMessageToHex(received), true, true);
    if (received.startsWith(check_result))
    {
        emit logD("Result check OK", true, true);
        result = kJ2534StatusNoerror;
    }
    else
    {
        emit logD("Result check failed, not maybe an j2534 interface!", true, true);
    }

    return result;
}

long J2534::PassThruClose(unsigned long device_id)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    // emit LOG_D("Close J2534 device ID:" << DeviceID;

    output = "atz\r\n";
    // emit LOG_D("Send data:" << output;
    WriteSerialData(output);
    received = ReadSerialData(8, 50);
    // emit LOG_D("Received:" << received;
    CloseSerialPort();

    return result;
}

long J2534::PassThruConnect(unsigned long device_id, unsigned long protocol_id, unsigned long flags,
                            unsigned long baudrate, unsigned long *p_channel_id)
{
    QByteArray output;
    QByteArray received;
    unsigned long chan_id = (unsigned long)p_channel_id;
    long result = kJ2534StatusNoerror;

    switch ((int)protocol_id)
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
    QString str = "ato" + QString::number(protocol_id) + " " + QString::number(flags) + " " +
                  QString::number(baudrate) + " " + QString::number(protocol_id) + "\r\n";
    output.append(str.toUtf8());
    // emit LOG_D("Send data:" << output;
    WriteSerialData(output);
    received = ReadSerialData(100, 50);
    emit logD("Connect received: " + ParseMessageToHex(received) + " " + received + " " + QString::number(chan_id),
              true, true);

    return result;
}

long J2534::PassThruDisconnect(unsigned long channel_id)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    // emit LOG_D("Disconnect J2534 device in channel:" << ChannelID;

    output.clear();
    QString str = "atc" + QString::number(channel_id) + "\r\n";
    output.append(str.toUtf8());
    // emit LOG_D("Send data:" << output;
    WriteSerialData(output);
    received = ReadSerialData(100, 50);
    // emit LOG_D("Received:" << received;

    return result;
}

long J2534::PassThruReadMsgs(unsigned long channel_id, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                             unsigned long timeout)
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

    received = ReadSerialData(3, timeout);
    // emit LOG_D("Recieved data: " + parseMessageToHex(received), true, true);
    while (received.length() > 0 && IsSerialPortOpen())
    {
        // emit LOG_D("Message header: " + parseMessageToHex(received), true, true);
        if (received.at(0) == 0x61 && received.at(1) == 0x72)
        {
            if (received.at(2) == 'o') // ACK 0x6f
            {
                ReadSerialData(2, timeout);
                // emit LOG_D("Sent msg ACK: " + parseMessageToHex(received), true, true);
                received.clear();
                msg_ack_ = true;
            }
            else if (received.at(2) == 'e') // error 0x65
            {
                while ((uint8_t)received.at(received.length() - 1) == 0x0d)
                {
                    received.append(ReadSerialData(1, timeout));
                }
                // emit LOG_D("Error sending message: " + parseMessageToHex(received), true, true);
                received.clear();
            }
            else if (received.at(2) == 'm') // 0x6d
            {
                received.append(ReadSerialData(2, timeout));
                msg.clear();
                while ((uint8_t)msg[msg.length() - 1] != 0x20)
                {
                    msg.append(ReadSerialData(1, timeout));
                }
                received.append(msg);
                periodic_msg_id_ = msg.remove(msg.length() - 1, 1).toULong();

                while ((uint8_t)msg[msg.length() - 1] != 0x0a)
                {
                    msg.append(ReadSerialData(1, timeout));
                }
                received.append(msg);
                msg = ReadSerialData(msg_byte_cnt, timeout);
                received.append(msg);

                msg_index = 0;
                // emit LOG_D("Periodic msg response: " + parseMessageToHex(received), true, true);
            }
            else if (received.at(2) == 'r') // 0x72
            {
                while ((uint8_t)received.at(received.length() - 1) != 0x0a)
                {
                    received.append(ReadSerialData(1, timeout));
                }
                for (int i = 0; i < received.length(); i++)
                {
                    p_msg->data[i] = (uint8_t)received.at(i);
                }
                p_msg->data_size = received.length();

                // emit LOG_D("vBatt msg response: " + parseMessageToHex(received), true, true);
                return kJ2534StatusNoerror;
            }
            else if (received.at(2) == 'w') // 0x77
            {
                while ((uint8_t)received.at(received.length() - 1) != 0x0a)
                {
                    received.append(ReadSerialData(1, timeout));
                }
                for (int i = 0; i < received.length(); i++)
                {
                    p_msg->data[i] = (uint8_t)received.at(i);
                }
                p_msg->data_size = received.length();

                // emit LOG_D("Five baud init msg response: " + parseMessageToHex(received), true, true);
                return kJ2534StatusNoerror;
            }
            else if (received.at(2) == 'y') // 0x79
            {
                received.append(ReadSerialData(2, timeout));
                msg.clear();
                while ((uint8_t)msg[msg.length() - 1] != 0x20)
                {
                    msg.append(ReadSerialData(1, timeout));
                }
                received.append(msg);
                msg_byte_cnt = msg.remove(msg.length() - 1, 1).toInt();

                while ((uint8_t)msg[msg.length() - 1] != 0x0a)
                {
                    msg.append(ReadSerialData(1, timeout));
                }
                received.append(msg);
                msg = ReadSerialData(msg_byte_cnt, timeout);
                received.append(msg);

                msg_index = 0;
                for (unsigned long i = 0; i < msg_byte_cnt; i++)
                {
                    p_msg->data[msg_index++] = (uint8_t)msg.at(static_cast<qsizetype>(i));
                }

                p_msg->rx_status = kNormMsg;
                p_msg->data_size = msg_index;
                msg_cnt++;
                // emit LOG_D("Fast init msg response: " + parseMessageToHex(received), true, true);
            }
            else if (received.at(2) == '3' || received.at(2) == '4' || received.at(2) == '5' || received.at(2) == '6')
            {
                received.append(ReadSerialData(2, timeout));
                msg_byte_cnt = received.at(3) - 1;
                msg_type = received.at(4);
                switch (msg_type)
                {
                case kNormMsg:
                    msg_type_string = "NORM_MSG";
                    break;
                case kJ2534StartOfMessage:
                    msg_type_string = "START_OF_MESSAGE";
                    break;
                case kTxDoneMsg:
                    msg_type_string = "TX_DONE_MSG";
                    break;
                case kTxLbMsg:
                    msg_type_string = "TX_LB_MSG";
                    break;
                case kRxMsgEndInd:
                    msg_type_string = "RX_MSG_END_IND";
                    break;
                case kExtAddrMsgEndInd:
                    msg_type_string = "EXT_ADDR_MSG_END_IND";
                    break;
                case kLbMsgEndInd:
                    msg_type_string = "LB_MSG_END_IND";
                    break;
                case kNormMsgStartInd:
                    msg_type_string = "NORM_MSG_START_IND";
                    break;
                case kTxLbStartInd:
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

                if (msg_type == kTxDoneMsg)
                {
                    p_msg->rx_status = kTxDoneMsg;
                    received.append(ReadSerialData(msg_byte_cnt, timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("TX_DONE_MSG: " + parseMessageToHex(received), true, true);
                    received.clear();
                    is_tx_done_ = true;
                }
                if (msg_type == kTxLbStartInd)
                {
                    p_msg->rx_status = kTxLbStartInd;
                    received.append(ReadSerialData(msg_byte_cnt, timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("TX_LB_START_IND: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == kTxLbMsg)
                {
                    p_msg->rx_status = kTxLbMsg;
                    received.append(ReadSerialData(msg_byte_cnt, timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("TX_LB_MSG: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == kLbMsgEndInd)
                {
                    p_msg->rx_status = kLbMsgEndInd;
                    received.append(ReadSerialData(msg_byte_cnt, timeout));
                    msg_index = 0;
                    msg_cnt = 0;
                    // emit LOG_D("LB_MSG_END_IND: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == kNormMsgStartInd)
                {
                    p_msg->rx_status = kNormMsgStartInd;
                    received.append(ReadSerialData(msg_byte_cnt, timeout));

                    msg_index = 0;
                    msg_cnt++;
                    chunk_cnt = 0;

                    // emit LOG_D("NORM_MSG_START_IND: " + parseMessageToHex(received), true, true);
                    received.clear();
                }
                if (msg_type == kNormMsg || msg_type == kJ2534StartOfMessage)
                {
                    p_msg->rx_status = kNormMsg;

                    received.append(ReadSerialData(msg_byte_cnt, timeout));
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
                            p_msg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 5);
                        }
                        if (received.at(2) == '5' || received.at(2) == '6')
                        {
                            if (chunk_cnt)
                            {
                                p_msg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 13);
                            }
                            else
                            {
                                p_msg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 9);
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
                        p_msg->timestamp = ParseTs(data.data());
                        p_msg->data_size = msg_index;
                        msg_cnt++;
                        stop_reading = true;
                    }
                    received.clear();
                }
                if (msg_type == kRxMsgEndInd)
                {
                    p_msg->rx_status = kRxMsgEndInd;

                    received.append(ReadSerialData(msg_byte_cnt, timeout));

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
                                p_msg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 13);
                            }
                            else
                            {
                                p_msg->data[msg_index++] = (uint8_t)received.at(static_cast<qsizetype>(i) + 9);
                            }
                        }
                    }
                    std::array<char, 4> data{};
                    data[0] = received.at(8);
                    data[1] = received.at(7);
                    data[2] = received.at(6);
                    data[3] = received.at(5);
                    p_msg->timestamp = ParseTs(data.data());
                    p_msg->data_size = msg_index;
                    msg_cnt++;

                    received.clear();
                    stop_reading = true;
                }
            }
        }
        if (!stop_reading)
        {
            QByteArray response = ReadSerialData(3, timeout);
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

    *p_num_msgs = msg_cnt;
    received.clear();
    return result;
}

long J2534::PassThruWriteMsgs(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_num_msgs,
                              unsigned long timeout)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    for (unsigned long msg_index = 0; msg_index < *p_num_msgs; msg_index++)
    {
        output.clear();
        QString str = "att" + QString::number(channel_id) + " " + QString::number(p_msg->data_size) + " " +
                      QString::number(p_msg->tx_flags) + "\r\n";
        output.append(str.toUtf8());
        for (unsigned long i = 0; i < p_msg->data_size; i++)
        {
            output.append(static_cast<char>(p_msg->data[i]));
        }
        WriteSerialData(output);
        p_msg++;
        is_tx_done_ = false;
    }

    return result;
}

long J2534::PassThruStartPeriodicMsg(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_msg_id,
                                     unsigned long time_interval)
{
    QByteArray output;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atm" + QString::number(channel_id) + " " + QString::number(time_interval * 1000) + " 0 " +
                  QString::number(p_msg->tx_flags) + " " + QString::number(p_msg->data_size) + "\r\n";
    output.append(str.toUtf8());
    for (unsigned long i = 0; i < p_msg->data_size; i++)
    {
        output.append(static_cast<char>(p_msg->data[i]));
    }

    WriteSerialData(output);
    // PassThruReadMsgs(ChannelID, &rxmsg, &numRxMsg, timeout);

    *p_msg_id = periodic_msg_id_;

    return result;
}

long J2534::PassThruStopPeriodicMsg(unsigned long channel_id, unsigned long msg_id)
{
    QByteArray output;
    long result = kJ2534StatusNoerror;

    QString str = "atn" + QString::number(channel_id) + " " + QString::number(msg_id) + "\r\n";
    output.append(str.toUtf8());

    WriteSerialData(output);

    return result;
}

long J2534::PassThruStartMsgFilter(unsigned long channel_id, unsigned long filter_type, const PassThruMsg *p_mask_msg,
                                   const PassThruMsg *p_pattern_msg, const PassThruMsg *p_flow_control_msg,
                                   unsigned long *p_msg_id)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atf" + QString::number(channel_id) + " " + QString::number(filter_type) + " " +
                  QString::number(p_mask_msg->tx_flags) + " " + QString::number(p_mask_msg->data_size);
    output.append(str.toUtf8());
    output.append("\r\n");

    for (unsigned long i = 0; i < p_mask_msg->data_size; i++)
    {
        output.append(static_cast<char>(p_mask_msg->data[i]));
    }
    for (unsigned long i = 0; i < p_pattern_msg->data_size; i++)
    {
        output.append(static_cast<char>(p_pattern_msg->data[i]));
    }
    if (p_flow_control_msg)
    {
        for (unsigned long i = 0; i < p_flow_control_msg->data_size; i++)
        {
            output.append(static_cast<char>(p_flow_control_msg->data[i]));
        }
    }
    // emit LOG_D("Send data:" << parseMessageToHex(output);
    WriteSerialData(output);
    received = ReadSerialData(100, 50);
    // emit LOG_D("Received:" << received;

    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long channel_id, unsigned long msg_id)
{
    long result = kJ2534StatusNoerror;

    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long device_id, unsigned long pin, unsigned long voltage)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atv" + QString::number(pin) + " " + QString::number(voltage) + "\r\n";
    output.append(str.toUtf8());
    WriteSerialData(output);
    // received = read_serial_data(5, 50);

    return result;
}

long J2534::PassThruReadVersion(char *p_api_version, char *p_dll_version, char *p_firmware_version,
                                unsigned long device_id)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    const std::size_t api_len = std::min(kApiVersion.size(), kVersionBufferSize - 1);
    std::memcpy(p_api_version, kApiVersion.data(), api_len);
    p_api_version[api_len] = '\0';
    const std::size_t dll_len = std::min(kDllVersion.size(), kVersionBufferSize - 1);
    std::memcpy(p_dll_version, kDllVersion.data(), dll_len);
    p_dll_version[dll_len] = '\0';
    // strncpy(pFirmwareVersion, fw_version, strlen(fw_version));

    output = "\r\n\r\nati\r\n";
    emit logD("Sent: " + ParseMessageToHex(output), true, true);
    WriteSerialData(output);
    Delay(50);
    received = ReadSerialData(50, 100);
    emit logD("Response: " + ParseMessageToHex(received), true, true);
    QString response = QString::fromUtf8(received);
    QStringList fw_ver = response.split("ari ");
    fw_ver = fw_ver.at(fw_ver.length() - 1).split("\r\n");
    // Hold the bytes in a named QByteArray: `fw_ver.at(0).toUtf8().data()` would
    // dangle (the temporary QByteArray is destroyed at the semicolon), leaving
    // pFirmwareVersion empty or holding garbage.
    const QByteArray fw_ver_bytes = fw_ver.at(0).toUtf8();
    const std::size_t fw_len = std::min<std::size_t>(fw_ver_bytes.size(), kVersionBufferSize - 1);
    std::memcpy(p_firmware_version, fw_ver_bytes.constData(), fw_len);
    p_firmware_version[fw_len] = '\0';

    return result;
}

long J2534::PassThruGetLastError(char *p_error_description)
{
    long result = kJ2534StatusNoerror;

    return result;
}

int J2534::IsValidSconfigParam(SCONFIG s)
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

void J2534::DumpSbyteArray(const SByteArray *s)
{
    // emit LOG_D("SByteArray size =" << s->NumOfBytes;
    // DBGPRINT(("SByteArray size=%u\n",s->NumOfBytes));
    // DBGDUMP((s->BytePtr,s->NumOfBytes,0));
}

void J2534::DumpSconfigParam(SCONFIG s)
{
    std::string param_name;

    switch (s.parameter)
    {
    case kJ2534DataRate:
        param_name = "DATA_RATE";
        break;
    case kJ2534Loopback:
        param_name = "LOOPBACK";
        break;
    case kJ2534NodeAddress:
        param_name = "NODE_ADDRESS";
        break;
    case kJ2534NetworkLine:
        param_name = "NETWORK_LINE";
        break;
    case kJ2534P1Min:
        param_name = "P1_MIN";
        break;
    case kJ2534P1Max:
        param_name = "P1_MAX";
        break;
    case kJ2534P2Min:
        param_name = "P2_MIN";
        break;
    case kJ2534P2Max:
        param_name = "P2_MAX";
        break;
    case kJ2534P3Min:
        param_name = "P3_MIN";
        break;
    case kJ2534P3Max:
        param_name = "P3_MAX";
        break;
    case kJ2534P4Min:
        param_name = "P4_MIN";
        break;
    case kJ2534P4Max:
        param_name = "P4_MAX";
        break;
    case kJ2534W1:
        param_name = "W1";
        break;
    case kJ2534W2:
        param_name = "W2";
        break;
    case kJ2534W3:
        param_name = "W3";
        break;
    case kJ2534W4:
        param_name = "W4";
        break;
    case kJ2534W5:
        param_name = "W5";
        break;
    case kJ2534Tidle:
        param_name = "TIDLE";
        break;
    case kJ2534Tinil:
        param_name = "TINIL";
        break;
    case kJ2534Twup:
        param_name = "TWUP";
        break;
    case kJ2534Parity:
        param_name = "PARITY";
        break;
    case kJ2534BitSamplePoint:
        param_name = "BIT_SAMPLE_POINT";
        break;
    case kJ2534SyncJumpWidth:
        param_name = "SYNC_JUMP_WIDTH";
        break;
    case kJ2534W0:
        param_name = "W0";
        break;
    case kJ2534T1Max:
        param_name = "T1_MAX";
        break;
    case kJ2534T2Max:
        param_name = "T2_MAX";
        break;
    case kJ2534T4Max:
        param_name = "T4_MAX";
        break;
    case kJ2534T5Max:
        param_name = "T5_MAX";
        break;
    case kJ2534Iso15765Bs:
        param_name = "ISO15765_BS";
        break;
    case kJ2534Iso15765Stmin:
        param_name = "ISO15765_STMIN";
        break;
    case kJ2534DataBits:
        param_name = "DATA_BITS";
        break;
    case kJ2534FiveBaudMod:
        param_name = "FIVE_BAUD_MOD";
        break;
    case kJ2534BsTx:
        param_name = "BS_TX";
        break;
    case kJ2534StminTx:
        param_name = "STMIN_TX";
        break;
    case kJ2534T3Max:
        param_name = "T3_MAX";
        break;
    case kJ2534Iso15765WftMax:
        param_name = "ISO15765_WFT_MAX";
        break;
    case kJ2534CanMixedFormat:
        param_name = "CAN_MIXED_FORMAT";
        break;
    case kJ2534J1962Pins:
        param_name = "J1962_PINS";
        break;
    case kJ2534SwCanHsDataRate:
        param_name = "W_CAN_HS_DATA_RATE";
        break;
    case kJ2534SwCanSpeedchangeEnable:
        param_name = "SW_CAN_SPEEDCHANGE_ENABLE";
        break;
    case kJ2534SwCanResSwitch:
        param_name = "SW_CAN_RES_SWITCH";
        break;
    case kJ2534ActiveChannels:
        param_name = "ACTIVE_CHANNELS";
        break;
    case kJ2534SampleRate:
        param_name = "SAMPLE_RATE";
        break;
    case kJ2534SamplesPerReading:
        param_name = "SAMPLES_PER_READING";
        break;
    case kJ2534ReadingsPerMsg:
        param_name = "READINGS_PER_MSG";
        break;
    case kJ2534AveragingMethod:
        param_name = "AVERAGING_METHOD";
        break;
    case kJ2534SampleResolution:
        param_name = "SAMPLE_RESOLUTION";
        break;
    case kJ2534InputRangeLow:
        param_name = "INPUT_RANGE_LOW";
        break;
    case kJ2534InputRangeHigh:
        param_name = "INPUT_RANGE_HIGH";
        break;
    default:
        param_name = std::format("{}(unknown)", s.parameter);
        break;
    }

    // DBGPRINT(("    %s : %u",paramName,s.Value));
    // emit LOG_D("    " << paramName << s.Value;
}

long J2534::PassThruIoctl(unsigned long channel_id, unsigned long ioctl_id, const void *p_input, void *p_output)
{
    QByteArray output;
    QByteArray received;
    uint32_t par_cnt = 0;

    int input_as_sa = 0;
    int output_as_sa = 0;
    unsigned int i;
    SConfigList *scl;
    long result = kJ2534StatusNoerror;
    std::string ioctl_name;

    // const SConfigList *inputlist = pInput;

    switch (ioctl_id)
    {
    case kJ2534GetConfig:
        ioctl_name = "GET_CONFIG";
        break;
    case kJ2534SetConfig:
        ioctl_name = "SET_CONFIG";
        break;
    case kJ2534ReadVbatt:
        ioctl_name = "READ_VBATT";
        break;
    case kJ2534FiveBaudInit:
        ioctl_name = "FIVE_BAUD_INIT";
        input_as_sa = 1;
        output_as_sa = 1;
        break;
    case kJ2534FastInit:
        ioctl_name = "FAST_INIT";
        break;
    case kJ2534ClearTxBuffer:
        ioctl_name = "CLEAR_TX_BUFFER";
        break;
    case kJ2534ClearRxBuffer:
        ioctl_name = "CLEAR_RX_BUFFER";
        break;
    case kJ2534ClearPeriodicMsgs:
        ioctl_name = "CLEAR_PERIODIC_MSGS";
        break;
    case kJ2534ClearMsgFilters:
        ioctl_name = "CLEAR_MSG_FILTERS";
        break;
    case kJ2534ClearFunctMsgLookupTable:
        ioctl_name = "CLEAR_FUNCT_MSG_LOOKUP_TABLE";
        break;
    case kJ2534AddToFunctMsgLookupTable:
        ioctl_name = "ADD_TO_FUNCT_MSG_LOOKUP_TABLE";
        break;
    case kJ2534DeleteFromFunctMsgLookupTable:
        ioctl_name = "DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE";
        break;
    case kJ2534ReadProgVoltage:
        ioctl_name = "READ_PROG_VOLTAGE";
        break;
        //    case TX_IOCTL_APP_SERVICE:
        //        strcpy(IoctlName,"APP_SERVICE");
        //        break;
    default:
        ioctl_name = std::format("{}(unknown)", ioctl_id);
        break;
    }

    if (ioctl_id == kJ2534GetConfig)
    {
    }
    if (ioctl_id == kJ2534SetConfig)
    {
        p_output = nullptr; // make some DLLs happy

        // dump params
        scl = (SConfigList *)p_input;
        for (i = 0; i < scl->num_of_params; i++)
        {
            DumpSconfigParam((scl->config_ptr)[i]);
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
            QString str = "ats" + QString::number(channel_id) + " " + QString::number(cfgitem_local->parameter) + " " +
                          QString::number(cfgitem_local->value) + "\r\n";
            output.append(str.toUtf8());
            WriteSerialData(output);
            emit logD("Sent: " + ParseMessageToHex(output), true, true);
            received = ReadSerialData(100, 50);
            emit logD("Response: " + ParseMessageToHex(received), true, true);
        }
    }
    if (ioctl_id == kJ2534ReadVbatt)
    {
        PassThruMsg rxmsg;
        unsigned long num_rx_msg;
        QByteArray received_local;
        rxmsg.data_size = 0;
        num_rx_msg = 1;

        unsigned long *v_batt = (unsigned long *)p_output;
        long pin = 16;
        output.clear();
        QString str = "atr " + QString::number((int)pin) + "\r\n";
        output.append(str.toUtf8());
        if (!IsSerialPortOpen())
        {
            return result;
        }
        if (serial_->bytesAvailable())
        {
            return result;
        }
        WriteSerialData(output);
        emit logD("Sent: " + ParseMessageToHex(output), true, true);
        result = PassThruReadMsgs(channel_id, &rxmsg, &num_rx_msg, serial_read_timeout_);
        if (result)
        {
            return result;
        }
        received_local.clear();
        for (unsigned long i_local = 0; i_local < rxmsg.data_size; i_local++)
        {
            received_local.append(static_cast<char>(rxmsg.data[i_local]));
        }
        emit logD("Response: " + ParseMessageToHex(received_local), true, true);
        QString response = QString(received_local).split(" ").at(QString(received_local).split(" ").length() - 1);
        response = response.split("\r\n").at(0);
        // emit LOG_D("Pin 16 voltage: " + response + " mV", true, true);
        *v_batt = response.toULong();
    }

    if (ioctl_id == kJ2534FiveBaudInit)
    {
        PassThruMsg rxmsg;
        unsigned long num_rx_msg;
        QByteArray received_local;
        rxmsg.data_size = 0;
        num_rx_msg = 1;
        SByteArray *msg = (SByteArray *)p_input;
        SByteArray *response = (SByteArray *)p_output;

        output.clear();
        QString str = "atw" + QString::number(channel_id) + " " + QString::number(msg->byte_ptr[0]) + " 0\r\n";
        output.append(str.toUtf8());
        WriteSerialData(output);
        emit logD("Sent: " + ParseMessageToHex(output), true, true);
        memset(&rxmsg, 0, sizeof(rxmsg));
        result = PassThruReadMsgs(channel_id, &rxmsg, &num_rx_msg, serial_read_extra_long_timeout_);
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
        emit logD("Response: " + ParseMessageToHex(received_local), true, true);
    }

    if (ioctl_id == kJ2534FastInit)
    {
        PassThruMsg *msg = (PassThruMsg *)p_input;

        output.clear();
        QString str = "aty" + QString::number(channel_id) + " " + QString::number(msg->data_size) + " 0\r\n";
        output.append(str.toUtf8());
        for (i = 0; i < msg->data_size; i++)
        {
            output.append(static_cast<char>(msg->data[i]));
        }
        WriteSerialData(output);
        emit logD("Sent: " + ParseMessageToHex(output), true, true);
    }

    if (input_as_sa)
    {
        // emit LOG_D("Input", true, true);
        DumpSbyteArray((SByteArray *)p_input);
    }

    // result = (*pfPassThruIoctl)(ChannelID,IoctlID,pInput,pOutput);

    if (output_as_sa)
    {
        // emit LOG_D("Output", true, true);
        DumpSbyteArray((SByteArray *)p_output);
    }

    return result;
}

void J2534::Delay(int n)
{
    QThread::msleep(n);
}

void J2534::handleError(QSerialPort::SerialPortError error)
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
        CloseSerialPort();
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
