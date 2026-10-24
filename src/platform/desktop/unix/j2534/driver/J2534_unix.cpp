#include "src/platform/desktop/unix/j2534/driver/J2534_unix.h"

#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace
{
QByteArray Payload(const PassThruMsg& msg)
{
    return {reinterpret_cast<const char *>(msg.data), static_cast<qsizetype>(msg.data_size)};
}
} // namespace

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

bool J2534::GetIsTxDone()
{
    return is_tx_done_;
}

QString J2534::ParseMessageToHex(const QByteArray& received)
{
    QByteArray msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg(static_cast<uint8_t>(received.at(i)), 2, 16, QLatin1Char('0')).toUtf8());
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
    QString name = static_cast<const char *>(p_name);
    unsigned long *dev_id = p_device_id;
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

long J2534::PassThruClose(unsigned long /*device_id*/)
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

long J2534::PassThruConnect(unsigned long /*device_id*/, unsigned long protocol_id, unsigned long flags,
                            unsigned long baudrate, unsigned long *p_channel_id)
{
    QByteArray output;
    QByteArray received;
    unsigned long chan_id = reinterpret_cast<unsigned long>(p_channel_id);
    long result = kJ2534StatusNoerror;

    switch (static_cast<int>(protocol_id))
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

long J2534::PassThruReadMsgs(unsigned long /*channel_id*/, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                             unsigned long timeout)
{
    RxState state{.msg = p_msg, .timeout = timeout};
    QByteArray received = ReadSerialData(3, timeout);
    while (received.length() > 0 && IsSerialPortOpen())
    {
        FrameOutcome outcome = FrameOutcome::kContinue;
        if (received.at(0) == 0x61 && received.at(1) == 0x72)
        {
            outcome = HandleAdapterFrame(received, state);
        }
        if (outcome == FrameOutcome::kReturn)
        {
            return kJ2534StatusNoerror;
        }
        if (outcome == FrameOutcome::kStop)
        {
            break;
        }
        const QByteArray response = ReadSerialData(3, timeout);
        if (response.isEmpty())
        {
            received.clear();
        }
        else
        {
            received.append(response);
        }
    }

    *p_num_msgs = state.msg_cnt;
    return kJ2534StatusNoerror;
}

// Dispatches one "ar<kind>" adapter frame. Frames that leave `received`
// uncleared are dispatched again, with the next three bytes appended.
J2534::FrameOutcome J2534::HandleAdapterFrame(QByteArray& received, RxState& state)
{
    switch (received.at(2))
    {
    case 'o': // ACK
        ReadSerialData(2, state.timeout);
        received.clear();
        msg_ack_ = true;
        return FrameOutcome::kContinue;
    case 'e': // error; its text stays queued and is skipped as an unknown header
        received.clear();
        return FrameOutcome::kContinue;
    case 'm': // periodic message started
        ReadSerialData(2, state.timeout);
        periodic_msg_id_ = ReadUntil(' ', state.timeout).chopped(1).toULong();
        ReadUntil('\n', state.timeout);
        ReadSerialData(state.msg_byte_cnt, state.timeout);
        state.msg_index = 0;
        return FrameOutcome::kContinue;
    case 'r': // battery voltage
    case 'w': // five-baud init
        while (static_cast<uint8_t>(received.back()) != 0x0a)
        {
            received.append(ReadSerialData(1, state.timeout));
        }
        std::copy(received.cbegin(), received.cend(), state.msg->data);
        state.msg->data_size = received.length();
        return FrameOutcome::kReturn;
    case 'y': // fast init
        ReadFastInitReply(state);
        return FrameOutcome::kContinue;
    case '3':
    case '4':
    case '5':
    case '6':
        return HandleRxFrame(received, state);
    default:
        return FrameOutcome::kContinue;
    }
}

QByteArray J2534::ReadUntil(char terminator, unsigned long timeout)
{
    QByteArray field;
    while (field.isEmpty() || field.back() != terminator)
    {
        field.append(ReadSerialData(1, timeout));
    }
    return field;
}

void J2534::ReadFastInitReply(RxState& state)
{
    ReadSerialData(2, state.timeout);
    state.msg_byte_cnt = ReadUntil(' ', state.timeout).chopped(1).toInt();
    ReadUntil('\n', state.timeout);
    const QByteArray reply = ReadSerialData(state.msg_byte_cnt, state.timeout);

    state.msg_index = 0;
    CopyPayload(reply, 0, state);
    state.msg->rx_status = kNormMsg;
    state.msg->data_size = state.msg_index;
    state.msg_cnt++;
}

// "ar3".."ar6": a received-message frame with a length byte and an RxMsgType.
J2534::FrameOutcome J2534::HandleRxFrame(QByteArray& received, RxState& state)
{
    received.append(ReadSerialData(2, state.timeout));
    state.msg_byte_cnt = received.at(3) - 1;
    const auto msg_type = static_cast<uint8_t>(received.at(4));
    switch (msg_type)
    {
    case kTxDoneMsg:
        is_tx_done_ = true;
        [[fallthrough]];
    case kTxLbStartInd:
    case kTxLbMsg:
    case kLbMsgEndInd:
        state.msg->rx_status = msg_type;
        ReadSerialData(state.msg_byte_cnt, state.timeout);
        state.msg_index = 0;
        state.msg_cnt = 0;
        received.clear();
        return FrameOutcome::kContinue;
    case kNormMsgStartInd:
        state.msg->rx_status = msg_type;
        ReadSerialData(state.msg_byte_cnt, state.timeout);
        state.msg_index = 0;
        state.msg_cnt++;
        state.chunk_cnt = 0;
        received.clear();
        return FrameOutcome::kContinue;
    case kJ2534StartOfMessage:
        state.msg_index = 0;
        state.msg_cnt++;
        [[fallthrough]];
    case kNormMsg:
        return HandleNormFrame(received, state);
    case kRxMsgEndInd:
        state.msg->rx_status = kRxMsgEndInd;
        received.append(ReadSerialData(state.msg_byte_cnt, state.timeout));
        if (received.at(2) == '6')
        {
            CopyTimestampedPayload(received, state);
        }
        return FinishMessage(received, state);
    default:
        return FrameOutcome::kContinue;
    }
}

J2534::FrameOutcome J2534::HandleNormFrame(QByteArray& received, RxState& state)
{
    state.msg->rx_status = kNormMsg;
    received.append(ReadSerialData(state.msg_byte_cnt, state.timeout));
    const char kind = received.at(2);
    if (kind == '5' || kind == '6')
    {
        CopyTimestampedPayload(received, state);
    }
    else
    {
        CopyPayload(received, 5, state);
    }
    state.chunk_cnt++;
    if (kind == '5')
    {
        return FinishMessage(received, state);
    }
    received.clear();
    return FrameOutcome::kContinue;
}

// '5' and '6' frames carry a 4-byte timestamp ahead of the payload; each
// later chunk of a multi-frame message carries 4 more bytes.
void J2534::CopyTimestampedPayload(const QByteArray& received, RxState& state)
{
    const unsigned long skipped = state.chunk_cnt ? 8 : 4;
    state.msg_byte_cnt -= skipped;
    CopyPayload(received, 5 + static_cast<qsizetype>(skipped), state);
}

void J2534::CopyPayload(const QByteArray& received, qsizetype offset, RxState& state)
{
    for (unsigned long i = 0; i < state.msg_byte_cnt; i++)
    {
        state.msg->data[state.msg_index++] = static_cast<uint8_t>(received.at(static_cast<qsizetype>(i) + offset));
    }
}

J2534::FrameOutcome J2534::FinishMessage(QByteArray& received, RxState& state)
{
    std::array<char, 4> data{received.at(8), received.at(7), received.at(6), received.at(5)};
    state.msg->timestamp = ParseTs(data.data());
    state.msg->data_size = state.msg_index;
    state.msg_cnt++;
    received.clear();
    return FrameOutcome::kStop;
}

long J2534::PassThruWriteMsgs(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_num_msgs,
                              unsigned long /*timeout*/)
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
        output.append(Payload(*p_msg));
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
    output.append(Payload(*p_msg));

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
                                   unsigned long * /*p_msg_id*/)
{
    QByteArray output;
    QByteArray received;
    long result = kJ2534StatusNoerror;

    output.clear();
    QString str = "atf" + QString::number(channel_id) + " " + QString::number(filter_type) + " " +
                  QString::number(p_mask_msg->tx_flags) + " " + QString::number(p_mask_msg->data_size);
    output.append(str.toUtf8());
    output.append("\r\n");

    output.append(Payload(*p_mask_msg));
    output.append(Payload(*p_pattern_msg));
    if (p_flow_control_msg)
    {
        output.append(Payload(*p_flow_control_msg));
    }
    // emit LOG_D("Send data:" << parseMessageToHex(output);
    WriteSerialData(output);
    received = ReadSerialData(100, 50);
    // emit LOG_D("Received:" << received;

    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long /*channel_id*/, unsigned long /*msg_id*/)
{
    long result = kJ2534StatusNoerror;

    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long /*device_id*/, unsigned long pin, unsigned long voltage)
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
                                unsigned long /*device_id*/)
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

long J2534::PassThruGetLastError(char * /*p_error_description*/)
{
    long result = kJ2534StatusNoerror;

    return result;
}

long J2534::PassThruIoctl(unsigned long channel_id, unsigned long ioctl_id, const void *p_input, void *p_output)
{
    switch (ioctl_id)
    {
    case kJ2534SetConfig:
        SendConfig(channel_id, *static_cast<const SConfigList *>(p_input));
        break;
    case kJ2534ReadVbatt:
        return ReadVbatt(channel_id, static_cast<unsigned long *>(p_output));
    case kJ2534FiveBaudInit:
        return FiveBaudInit(channel_id, *static_cast<const SByteArray *>(p_input),
                            *static_cast<SByteArray *>(p_output));
    case kJ2534FastInit:
        SendFastInit(channel_id, *static_cast<const PassThruMsg *>(p_input));
        break;
    default:
        break;
    }
    return kJ2534StatusNoerror;
}

// Enabling a check against IsValidSconfigParam here could break some J2534
// devices such as Denso DST-i, so every parameter is passed through.
void J2534::SendConfig(unsigned long channel_id, const SConfigList& config)
{
    for (const SCONFIG& item : std::span(config.config_ptr, config.num_of_params))
    {
        const QByteArray output = ("ats" + QString::number(channel_id) + " " + QString::number(item.parameter) + " " +
                                   QString::number(item.value) + "\r\n")
                                      .toUtf8();
        WriteSerialData(output);
        emit logD("Sent: " + ParseMessageToHex(output), true, true);
        emit logD("Response: " + ParseMessageToHex(ReadSerialData(100, 50)), true, true);
    }
}

long J2534::ReadVbatt(unsigned long channel_id, unsigned long *v_batt)
{
    if (!IsSerialPortOpen() || serial_->bytesAvailable())
    {
        return kJ2534StatusNoerror;
    }
    const QByteArray output = "atr 16\r\n"; // OBD pin 16: battery voltage
    WriteSerialData(output);
    emit logD("Sent: " + ParseMessageToHex(output), true, true);

    PassThruMsg rxmsg{};
    unsigned long num_rx_msg = 1;
    if (const long result = PassThruReadMsgs(channel_id, &rxmsg, &num_rx_msg, serial_read_timeout_); result)
    {
        return result;
    }
    const QByteArray reply = Payload(rxmsg);
    emit logD("Response: " + ParseMessageToHex(reply), true, true);
    // "arr<pin> <millivolts>\r\n"
    *v_batt = QString::fromUtf8(reply).split(" ").last().split("\r\n").first().toULong();
    return kJ2534StatusNoerror;
}

long J2534::FiveBaudInit(unsigned long channel_id, const SByteArray& address, SByteArray& response)
{
    const QByteArray output =
        ("atw" + QString::number(channel_id) + " " + QString::number(address.byte_ptr[0]) + " 0\r\n").toUtf8();
    WriteSerialData(output);
    emit logD("Sent: " + ParseMessageToHex(output), true, true);

    PassThruMsg rxmsg{};
    unsigned long num_rx_msg = 1;
    if (const long result = PassThruReadMsgs(channel_id, &rxmsg, &num_rx_msg, serial_read_extra_long_timeout_); result)
    {
        return result;
    }
    std::copy_n(rxmsg.data, rxmsg.data_size, response.byte_ptr);
    response.num_of_bytes = rxmsg.data_size;
    emit logD("Response: " + ParseMessageToHex(Payload(rxmsg)), true, true);
    return kJ2534StatusNoerror;
}

void J2534::SendFastInit(unsigned long channel_id, const PassThruMsg& request)
{
    const QByteArray output =
        ("aty" + QString::number(channel_id) + " " + QString::number(request.data_size) + " 0\r\n").toUtf8() +
        Payload(request);
    WriteSerialData(output);
    emit logD("Sent: " + ParseMessageToHex(output), true, true);
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
