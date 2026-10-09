// This is an open source non-commercial project. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com

#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

#include <QThread>

#include <algorithm>
#include <array>
#include <string_view>

#include "src/algorithms/protocol/fixed_buffer.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

namespace
{
// Length of the next J2534 message: what is left to send, capped at what one
// PassThruMsg can carry. The cap bounds the result, so the narrowing to long
// (32 bits on Windows) is lossless.
long NextPassThruChunkLength(const QByteArray& remaining)
{
    return static_cast<long>(std::min<qsizetype>(remaining.length(), kJ2534PassthruMsgDataSize));
}

// RAII counter: marks a J2534 read as in-flight so a reentrant teardown
// (close_j2534_serial_port) won't free j2534 underneath it.
struct J2534IoScope
{
    int& depth;
    explicit J2534IoScope(int& d) : depth(d)
    {
        ++depth;
    }
    ~J2534IoScope()
    {
        --depth;
    }
};

// Binds an SConfigList to the array backing it, so NumOfParams cannot drift
// from the data it counts. The cast is here rather than at each call site
// because unsigned long is narrower than size_t on Windows.
template <std::size_t N> SConfigList ConfigList(std::array<SCONFIG, N>& params)
{
    return {static_cast<unsigned long>(params.size()), params.data()};
}

// The J2534 flag constants retain type int; these give them the unsigned
// type of the PassThruMsg and PassThruConnect fields they are combined with.
constexpr unsigned long kTxDone = kJ2534TxDone;
constexpr unsigned long kStartOfMessage = kJ2534StartOfMessage;
constexpr unsigned long kIso15765FramePad = kJ2534Iso15765FramePad;
constexpr unsigned long kIso9141NoChecksum = kJ2534Iso9141NoChecksum;
constexpr unsigned long kCanIdBoth = kJ2534CanIdBoth;
} // namespace

SerialPortActionsDirect::SerialPortActionsDirect(QObject *parent) : QObject(parent), serial_(new QSerialPort(this))
{
    j2534_ = new J2534();
    ConnectJ2534Logs();
}

SerialPortActionsDirect::~SerialPortActionsDirect()
{
    delete j2534_;
    delete serial_;
}

bool SerialPortActionsDirect::IsSerialPortOpen()
{
    if (!serial_->isOpen())
    {
        if (!j2534_init_ok_)
        {
            return false;
        }
        else
        {
            return j2534_ && j2534_->IsSerialPortOpen();
        }
    }

    return serial_->isOpen();
}

bool SerialPortActionsDirect::SetKlineTimings(uint32_t parameter, int value)
{
    p1_max_ms = value;
    return kSerialSuccess;
}

int SerialPortActionsDirect::ChangePortSpeed(QString port_speed)
{
    serial_port_baudrate = port_speed;
    baudrate_ = port_speed.toInt();

    emit logD("Changing baudrate, checking if port is open...", true, true);
    if (IsSerialPortOpen())
    {
        emit logD("Port is open, checking adapter type...", true, true);
        if (!use_openport2_adapter)
        {
            emit logD("Adapter type is generic OBD2...", true, true);

            if (serial_->setBaudRate(static_cast<qint32>(serial_port_baudrate.toDouble())))
            {
                delay(50);
                emit logD("Baudrate set to " + port_speed + " OK", true, true);
                return kSerialSuccess;
            }
            else
            {
                emit logE("ERROR setting baudrate!", true, true);
                return kSerialError;
            }
        }
        else
        {
            emit logD("Adapter type is J2534...", true, true);

            auto scp = std::to_array<SCONFIG>({{.parameter = kJ2534DataRate, .value = baudrate_}});
            SConfigList scl = ConfigList(scp);
            if (!j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
            {
                emit logD("Baudrate set to " + port_speed + " OK", true, true);
                delay(50);
                return kSerialSuccess;
            }
            else
            {
                ReportJ2534Error();
                return kSerialError;
            }
        }
    }

    return kSerialError;
}

QByteArray SerialPortActionsDirect::FiveBaudInit(QByteArray output)
{
    QByteArray response;

    if (use_openport2_adapter)
    {
        unsigned long result;
        SByteArray input_msg;
        SByteArray output_msg;

        std::array<unsigned char, 20> byte_ptr{};

        memset(&input_msg, 0, sizeof(input_msg));
        memset(&output_msg, 0, sizeof(output_msg));

        input_msg.num_of_bytes = 1;
        input_msg.byte_ptr = byte_ptr.data();
        output_msg.num_of_bytes = 0;
        output_msg.byte_ptr = byte_ptr.data();

        for (int i = 0; i < output.length(); i++)
        {
            byte_ptr[i] = (uint8_t)output.at(i);
        }

        result = j2534_->PassThruIoctl(chan_id_, kJ2534FiveBaudInit, &input_msg, &output_msg);
        if (result)
        {
            ReportJ2534Error();
            return response;
        }
        for (unsigned long i = 0; i < output_msg.num_of_bytes; i++)
        {
            response.append(static_cast<char>(output_msg.byte_ptr[i]));
        }
    }
    else
    {
        // Set timeout to 350ms before init
        accurateDelay(350);
        // Set break to set seril line low
        serial_->setBreakEnabled(true);
        // Set timeout to 200ms to generate 200ms low pulse
        accurateDelay(200);
        // Unset break to set seril line high
        serial_->setBreakEnabled(false);
        // Set timeout to 400ms to generate 400ms high pulse
        accurateDelay(400);
        serial_->setBreakEnabled(true);
        accurateDelay(400);
        serial_->setBreakEnabled(false);
        accurateDelay(400);
        serial_->setBreakEnabled(true);
        accurateDelay(400);
        serial_->setBreakEnabled(false);
        accurateDelay(400);
        // Set timeout to 400ms to generate 400ms high pulse before init data is sent

        // Send init data
        WriteSerialDataEchoCheck(output);
        response = ReadSerialObdData(40);
        // emit LOG_D("Read response", true, true);
        response = ReadSerialObdData(200);
        // emit LOG_D("Five baud init response: " + parse_message_to_hex(response), true, true);
        if ((uint8_t)response.at(1) == 0x08 && (uint8_t)response.at(2) == 0x08)
        {
            // delay(30);
            output.clear();
            output.append(static_cast<char>(~static_cast<uint8_t>(response.at(2))));
            WriteSerialDataEchoCheck(output);
            response.append(ReadSerialObdData(200));
        }
    }

    return response;
}

int SerialPortActionsDirect::FastInit(QByteArray output)
{
    QByteArray received;

    if (use_openport2_adapter)
    {
        unsigned long result;
        PassThruMsg input_msg;
        PassThruMsg output_msg;

        memset(&input_msg, 0, sizeof(input_msg));
        memset(&output_msg, 0, sizeof(output_msg));

        input_msg.protocol_id = kJ2534Iso14230;
        input_msg.tx_flags = 0;

        if (add_ssm_header)
        {
            output = AppendSsmHeader(output);
        }
        else if (add_iso9141_header)
        {
            output = AppendIso9141Header(output);
        }
        else if (add_iso14230_header)
        {
            output = AppendIso14230Header(output);
        }

        for (int i = 0; i < output.length(); i++)
        {
            input_msg.data[i] = (uint8_t)output.at(i);
        }
        input_msg.data_size = output.length();

        /* Set timeout to 350ms before init */
        accurateDelay(350);

        result = j2534_->PassThruIoctl(chan_id_, kJ2534FastInit, &input_msg, &output_msg);
        if (result)
        {
            ReportJ2534Error();
            return kSerialError;
        }
    }
    else
    {
        // Set timeout to 350ms before init
        accurateDelay(350);
        // Set break to set seril line low
        serial_->setBreakEnabled(true);
        // Set timeout to 25ms to generate 25ms low pulse
        accurateDelay(23.7);
        // Unset break to set seril line high
        serial_->setBreakEnabled(false);
        // Set timeout to 25ms to generate 25ms high pulse before init data is sent
        accurateDelay(23.8);
        // Send init data
        WriteSerialDataEchoCheck(output);
        received = ReadSerialData(10);
        // emit LOG_D("Fast init response: " + parse_message_to_hex(received), true, true);
        delay(100);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::ClearRxBuffer()
{
    if (use_openport2_adapter)
    {
        unsigned long status;

        status = j2534_->PassThruIoctl(chan_id_, kJ2534ClearRxBuffer, nullptr, nullptr);
        if (status)
        {
            ReportJ2534Error();
            return kSerialError;
        }
        else
        {
            emit logD("RX BUFFER EMPTY", true, true);
        }
    }
    else if (serial_->isOpen())
    {
        // Plain-serial counterpart of the J2534 CLEAR_RX_BUFFER ioctl above:
        // discard whatever's already landed in QSerialPort's read buffer so a
        // stale/junk byte from a previous exchange doesn't corrupt the next
        // read_serial_data() call.
        serial_->clear(QSerialPort::Input);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::ClearTxBuffer()
{
    if (use_openport2_adapter)
    {
        unsigned long status;

        status = j2534_->PassThruIoctl(chan_id_, kJ2534ClearTxBuffer, nullptr, nullptr);
        if (status)
        {
            ReportJ2534Error();
            return kSerialError;
        }
        else
        {
            emit logD("TX BUFFER EMPTY", true, true);
        }
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetLecLines(int lec1_state, int lec2_state)
{
    LineEndCheck1Toggled(lec1_state);
    LineEndCheck2Toggled(lec2_state);

    return kSerialSuccess;
}

int SerialPortActionsDirect::PulseLec1Line(int timeout_arg)
{
    LineEndCheck1Toggled(request_to_send_enabled);
    accurateDelay(timeout_arg);
    LineEndCheck1Toggled(request_to_send_disabled);
    // delay(timeout);

    // read_serial_data(100, 50);

    return kSerialSuccess;
}

int SerialPortActionsDirect::PulseLec2Line(int timeout_arg)
{
    LineEndCheck2Toggled(data_terminal_enabled);
    accurateDelay(timeout_arg);
    LineEndCheck2Toggled(data_terminal_disabled);
    // delay(timeout);

    // read_serial_data(100, 50);

    return kSerialSuccess;
}

int SerialPortActionsDirect::LineEndCheck1Toggled(int state)
{
    if (state == request_to_send_enabled)
    {
        if (use_openport2_adapter)
        {
            j2534_->PassThruSetProgrammingVoltage(dev_id_, kJ2534J1962Pin11, 12000);
            SettleAfterProgrammingVoltage();
        }
        else
        {
            serial_->setRequestToSend(request_to_send_enabled);
            set_request_to_send = false;
        }
    }
    else
    {
        if (use_openport2_adapter)
        {
            j2534_->PassThruSetProgrammingVoltage(dev_id_, kJ2534J1962Pin11, -2);
        }
        else
        {
            serial_->setRequestToSend(request_to_send_disabled);
            set_request_to_send = true;
        }
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::LineEndCheck2Toggled(int state)
{
    if (state == data_terminal_enabled)
    {
        if (use_openport2_adapter)
        {
            j2534_->PassThruSetProgrammingVoltage(dev_id_, kJ2534J1962Pin9, 12000);
            SettleAfterProgrammingVoltage();
        }
        else
        {
            serial_->setDataTerminalReady(data_terminal_enabled);
            set_data_terminal_ready = false;
        }
    }
    else
    {
        if (use_openport2_adapter)
        {
            j2534_->PassThruSetProgrammingVoltage(dev_id_, kJ2534J1962Pin9, -2);
        }
        else
        {
            serial_->setDataTerminalReady(data_terminal_disabled);
            set_data_terminal_ready = true;
        }
    }

    return kSerialSuccess;
}

QStringList SerialPortActionsDirect::CheckSerialPorts()
{
    const auto serial_ports_info = QSerialPortInfo::availablePorts();
    QStringList serial_ports;
    // QString j2534DllName = "j2534.dll";

    serial_port_available = false;

    for (const QSerialPortInfo& serial_port_info : serial_ports_info)
    {
        serial_ports.append(serial_port_info.portName() + " - " + serial_port_info.description());
        emit logD("Serial port name: " + serial_port_info.portName() + " " + serial_port_info.description(), true,
                  true);
    }
    std::sort(serial_ports.begin(), serial_ports.end(), std::less<QString>());

    AppendJ2534Interfaces(serial_ports);

    return serial_ports;
}

QString SerialPortActionsDirect::OpenSerialPort()
{
    emit logD("Serial port = " + serial_port_list.join(", "), true, true);
    // QString serial_port_text = serial_port_list.at(1);
    const ResolvedPort resolved = ResolvePort(serial_port_list.at(0));
    serial_port = resolved.port;
    emit logD("Interface: " + serial_port, true, true);

    if (!serial_port.isEmpty() && resolved.is_j2534)
    {
        ResetConnection();
        // close_serial_port();
        // use_openport2_adapter = true;

        j2534_is_denso_dsti_ = serial_port.contains("DST-i");

        // QMap<QString, QString> user_j2534_drivers; // Local drivers in software folder
        SelectJ2534Dll();
        long result;

        emit logD("Testing j2534 interface, please wait...", true, true);
        result = InitJ2534Connection();

        if (result == kSerialSuccess)
        {
            emit logD("J2534: Interface opened succesfully!", true, true);
            use_openport2_adapter = true;
            j2534_init_ok_ = true;
            j2534_->j2534_init_ok = true;
            opened_serial_port = serial_port;
        }
        else
        {
            emit logD("J2534: Failed to open interface!", true, true);
            use_openport2_adapter = false;
            ResetConnection();
        }
    }
    if (!use_openport2_adapter && opened_serial_port != serial_port)
    {
        emit logD("Testing serial interface, please wait...", true, true);
        // close_serial_port();

        serial_port = serial_port.split(" - ").at(0);

        if (!(serial_->isOpen() && serial_->isWritable()))
        {
            serial_->setPortName(serial_port);
            serial_->setBaudRate(static_cast<qint32>(serial_port_baudrate.toDouble()));
            serial_->setDataBits(QSerialPort::Data8);
            serial_->setStopBits(QSerialPort::OneStop);
            // serial->setParity(QSerialPort::EvenParity);
            serial_->setParity((QSerialPort::Parity)serial_port_parity);
            serial_->setFlowControl(QSerialPort::NoFlowControl);

            if (serial_->open(QIODevice::ReadWrite))
            {
                // serial->setDataTerminalReady(setDataTerminalReady);
                // serial->setRequestToSend(setRequestToSend);
                serial_->clearError();
                serial_->clear();
                serial_->flush();
                opened_serial_port = serial_port;
                // connect(serial, SIGNAL(readyRead()), this, SLOT(ReadSerialDataSlot()), Qt::DirectConnection);
                qRegisterMetaType<QSerialPort::SerialPortError>();
                connect(serial_, SIGNAL(errorOccurred(QSerialPort::SerialPortError)), this,
                        SLOT(handleError(QSerialPort::SerialPortError)));

                emit logD("Serial port '" + serial_port + "' is open at baudrate " + serial_port_baudrate, true, true);
                return opened_serial_port;
            }
            else
            {
                emit logE("Couldn't open serial port '" + serial_port + "'", true, true);
                return {};
            }
        }
        else
        {
            emit logD("Serial port '" + serial_port + "' is already opened", true, true);
            return opened_serial_port;
        }
    }

    return opened_serial_port;
}

void SerialPortActionsDirect::ResetConnection()
{
    CloseJ2534SerialPort();
    closeSerialPort();
    delay(250);
}

void SerialPortActionsDirect::closeSerialPort()
{
    if (serial_->isOpen())
    {
        serial_->close();
        opened_serial_port.clear();
    }
}

void SerialPortActionsDirect::CloseJ2534SerialPort()
{
    if (j2534_io_depth_ > 0)
    {
        // A J2534 read is in-flight (its read paths pumped the event loop and we
        // were re-entered). Freeing j2534 now would pull the object out from under
        // that in-flight read. Skip the teardown; it can run once the read returns.
        emit logD("Skipping J2534 reset: a read is in-flight (reentrancy guard)", true, true);
        return;
    }
    if (j2534_->IsSerialPortOpen())
    {
        bool j2534_disconnect_ok = false;
        bool j2534_close_ok = false;
        for (int i = 0; i < 5; i++)
        {
            if (!j2534_->PassThruDisconnect(chan_id_))
            {
                j2534_disconnect_ok = true;
                break;
            }
            delay(100);
        }
        if (!j2534_disconnect_ok)
        {
            emit logD("J2534 interface disconnect failed!", true, true);
        }
        else
        {
            emit logD("J2534 interface disconnected succesfully!", true, true);
        }
        for (int i = 0; i < 5; i++)
        {
            if (!j2534_->PassThruClose(dev_id_))
            {
                j2534_close_ok = true;
                break;
            }
            delay(100);
        }
        if (!j2534_close_ok)
        {
            emit logD("J2534 interface close failed!", true, true);
        }
        else
        {
            emit logD("J2534 interface closed succesfully!", true, true);
        }
    }
    use_openport2_adapter = false;
    j2534_open_ok_ = false;
    j2534_get_version_ok_ = false;
    j2534_connect_ok_ = false;
    j2534_timing_ok_ = false;
    j2534_filters_ok_ = false;
    j2534_init_ok_ = false;
    j2534_->j2534_init_ok = false;
    opened_serial_port.clear();
    std::array<char, 256> dll_name{};
    j2534_->GetDllName(dll_name.data());
    delete j2534_;
    // Null the pointer across the delay(): delay() pumps the event loop, so a
    // reentrant read can run here. A guarded read sees null (safe) instead of a
    // dangling pointer (use-after-free).
    j2534_ = nullptr;
    delay(100);
    j2534_ = new J2534();
    j2534_->SetDllName(dll_name.data());
    ConnectJ2534Logs();
}

QByteArray SerialPortActionsDirect::SetError()
{
    QByteArray received;

    received.append(static_cast<char>(0x80));
    received.append(static_cast<char>(0xf0));
    received.append((uint8_t)0x10);
    received.append((uint8_t)0x03);
    received.append((uint8_t)0x7f);
    received.append('\0');
    received.append((uint8_t)0x13);

    return received;
}

QByteArray SerialPortActionsDirect::ReadSerialObdData(uint16_t timeout_arg)
{
    QByteArray received;

    if (IsSerialPortOpen())
    {
        if (use_openport2_adapter)
        {
            received = ReadJ2534Data(timeout_arg);
            return received;
        }

        // emit LOG_D("Check bytes available", true, true);
        QTime die_time = QTime::currentTime().addMSecs(timeout_arg);
        while (!serial_->bytesAvailable() && QTime::currentTime() < die_time)
        {
            serial_->waitForReadyRead(1);
        }
        // emit LOG_D("Byte(s) available or timeout", true, true);
        if (serial_->bytesAvailable())
        {
            // emit LOG_D("Byte(s) available", true, true);
            QTime interval_time = QTime::currentTime().addMSecs(p1_max_ms);
            while (QTime::currentTime() < die_time)
            {
                if (serial_->bytesAvailable())
                {
                    // emit LOG_D("Byte available", true, true);
                    received.append(serial_->read(1));
                    interval_time = QTime::currentTime().addMSecs(p1_max_ms);
                }
                if (interval_time < QTime::currentTime())
                {
                    // emit LOG_D("Byte timeout", true, true);
                    break;
                }
                serial_->waitForReadyRead(1);
            }
            // if (QTime::currentTime() > dieTime)
            //     emit LOG_D("Message timeout", true, true);
        }
    }
    return received;
}

QByteArray SerialPortActionsDirect::ReadSerialData(uint16_t timeout_arg)
{
    QByteArray received;
    QByteArray req_bytes;
    uint32_t msglen = 0;

    if (IsSerialPortOpen())
    {
        if (use_openport2_adapter)
        {
            received = ReadJ2534Data(timeout_arg);
            return received;
        }

        // emit LOG_D("Check if bytes available", true, true);
        received.clear();
        QTime die_time = QTime::currentTime().addMSecs(timeout_arg);
        while (!serial_->bytesAvailable() && QTime::currentTime() < die_time)
        {
            serial_->waitForReadyRead(1);
        }
        if (serial_->bytesAvailable())
        {
            QByteArray error_bytes;
            while (received.length() < 4 && QTime::currentTime() < die_time)
            {
                while (serial_->bytesAvailable() && received.length() < 4)
                {
                    received.append(serial_->read(1));
                }
                if (!is_iso14230_connection)
                {
                    // emit LOG_D("Check for valid header", true, true);
                    error_bytes.clear();
                    while (received.length() > 2 && !received.startsWith("\xbe\xef") &&
                           !received.startsWith("\x80\xf0\x10") && !received.startsWith("\x80\xf0\x01"))
                    {
                        error_bytes.append(received.mid(0, 1));
                        received.remove(0, 1);
                    }
                    // emit LOG_D("Error bytes length: " + QString::number(error_bytes.length()) + " : " +
                    // parse_message_to_hex(error_bytes), true, true);
                }
                serial_->waitForReadyRead(1);
            }
            // emit LOG_D("1. Response (header): " + parse_message_to_hex(received), true, true);
            if (is_iso14230_connection)
            {
                // emit LOG_D("Read with ISO14230", true, true);

                if (const auto format = static_cast<uint8_t>(received.at(0)); format & 0x3fU)
                {
                    msglen = format & 0x3fU; // Byte in index 3 is payload, no +1 for checksum
                }
                else
                {
                    msglen = received.at(3) + 1; // +1 for checksum
                }
            }
            else if (!is_iso14230_connection)
            {
                if (received.startsWith("\xbe\xef"))
                {
                    msglen = bytes::ReadU16Be(bytes::View(received), 2) + 1; // +1 for checksum
                }
                if (received.startsWith("\x80\xf0"))
                {
                    msglen = (uint8_t)received.at(3) + 1; // +1 for checksum
                }
            }
            while ((uint32_t)req_bytes.length() < msglen && QTime::currentTime() < die_time)
            {
                while (serial_->bytesAvailable() && (uint32_t)req_bytes.length() < msglen)
                {
                    req_bytes.append(serial_->read(1));
                }
                serial_->waitForReadyRead(1);
            }
            // emit LOG_D("2. Response (payload): " + parse_message_to_hex(req_bytes), true, true);
        }
        if (!received.length())
        {
            // emit LOG_D("No message received!", true, true);
        }
        else if (received.length() < 4 || (received.length() && (uint32_t)req_bytes.length() < msglen))
        {
            received.insert(0, SetError());
            received.append(req_bytes);
            // emit LOG_D("Message too short: " + parse_message_to_hex(received), true, true);
            return received;
        }
        received.append(req_bytes);
        // emit LOG_D("3. Response (full): " + parse_message_to_hex(received), true, true);

        return received;
    }
    return received;
}

QByteArray SerialPortActionsDirect::WriteSerialData(QByteArray output)
{
    QByteArray received;
    QByteArray msg;

    msg.append('\0');

    if (IsSerialPortOpen())
    {
        if (add_ssm_header)
        {
            output = AppendSsmHeader(output);
        }
        else if (add_iso9141_header)
        {
            output = AppendIso9141Header(output);
        }
        else if (add_iso14230_header)
        {
            output = AppendIso14230Header(output);
        }

        if (use_openport2_adapter)
        {
            WriteJ2534Data(output);
            return {};
        }

        while (serial_->bytesAvailable())
        {
            received.append(serial_->readAll());
        }

        for (int i = 0; i < output.length(); i++)
        {
            msg[0] = output.at(i);
            serial_->write(msg, 1);
        }
        received.clear();
        return {};
    }
    return {};
}

QByteArray SerialPortActionsDirect::WriteSerialDataEchoCheck(QByteArray output)
{
    QByteArray received;
    QByteArray msg;

    msg.append('\0');

    if (IsSerialPortOpen())
    {
        if (add_ssm_header)
        {
            output = AppendSsmHeader(output);
        }
        else if (add_iso9141_header)
        {
            output = AppendIso9141Header(output);
        }
        else if (add_iso14230_header)
        {
            output = AppendIso14230Header(output);
        }

        if (use_openport2_adapter)
        {
            WriteJ2534Data(output);
            return {};
        }

        while (serial_->bytesAvailable())
        {
            received.append(serial_->readAll());
        }

        received.clear();
        for (int i = 0; i < output.length(); i++)
        {
            msg[0] = output.at(i);
            serial_->write(msg, 1);
            // Add serial echo read during transmit to speed up a little
            if (serial_->bytesAvailable())
            {
                received.append(serial_->read(1));
            }
        }
        QTime die_time = QTime::currentTime().addMSecs(echo_check_timout);
        while (received.length() < output.length() && (QTime::currentTime() < die_time))
        {
            while (serial_->bytesAvailable() && received.length() < output.length())
            {
                die_time = QTime::currentTime().addMSecs(echo_check_timout);
                received.append(serial_->read(1));
            }
            serial_->waitForReadyRead(1);
        }
        if (received.length() < output.length())
        {
            emit logD("Write serial data echo read failed!", true, true);
        }

        received.clear();
        return {};
    }
    return {};
}

QByteArray SerialPortActionsDirect::AppendSsmHeader(QByteArray output)
{
    uint8_t chk_sum = 0;
    uint8_t msglength = output.length();

    output.insert(0, static_cast<char>(kline_startbyte));
    output.insert(1, static_cast<char>(kline_target_id));
    output.insert(2, static_cast<char>(kline_tester_id));
    output.insert(3, static_cast<char>(msglength));

    for (int i = 0; i < output.length(); i++)
    {
        chk_sum = chk_sum + output.at(i);
    }

    output.append(static_cast<char>(chk_sum));

    // LOG_D("Generated iso9141 message: " + parse_message_to_hex(output), true, true);

    return output;
}

QByteArray SerialPortActionsDirect::AppendIso9141Header(QByteArray output)
{
    uint8_t chk_sum = 0;

    output.insert(0, static_cast<char>(kline_startbyte));
    output.insert(1, static_cast<char>(kline_target_id));
    output.insert(2, static_cast<char>(kline_tester_id));

    for (int i = 0; i < output.length(); i++)
    {
        chk_sum = chk_sum + output.at(i);
    }

    output.append(static_cast<char>(chk_sum));

    // LOG_D("Generated iso9141 message: " + parse_message_to_hex(output), true, true);

    return output;
}

QByteArray SerialPortActionsDirect::AppendIso14230Header(QByteArray output)
{
    uint8_t chk_sum = 0;
    uint8_t msglength = output.length();

    // emit LOG_D("Adding iso14230 header to message", true, true);

    output.insert(0, static_cast<char>(kline_startbyte));
    output.insert(1, static_cast<char>(kline_target_id));
    output.insert(2, static_cast<char>(kline_tester_id));
    if (msglength < 0x40)
    {
        output[0] = static_cast<char>(static_cast<uint8_t>(output[0]) | msglength);
    }
    else
    {
        output.insert(3, static_cast<char>(msglength));
    }

    for (int i = 0; i < output.length(); i++)
    {
        chk_sum = chk_sum + output.at(i);
    }

    output.append(static_cast<char>(chk_sum));

    // LOG_D("Generated iso14230 message: " + parse_message_to_hex(output), true, true);

    return output;
}

int SerialPortActionsDirect::WriteJ2534Data(QByteArray output)
{
    PassThruMsg txmsg;
    unsigned long num_msgs;
    long tx_msg_len = NextPassThruChunkLength(output);

    while (tx_msg_len > 0)
    {
        txmsg.protocol_id = protocol_;
        txmsg.rx_status = 0;
        txmsg.tx_flags = 0;
        if (protocol_ == kJ2534Can)
        {
            if (is_29_bit_id)
            {
                txmsg.tx_flags = kJ2534Can29BitId;
            }
        }
        txmsg.tx_flags |= kIso15765FramePad;
        txmsg.timestamp = 0;
        txmsg.data_size = tx_msg_len;
        txmsg.extra_data_index = 0;

        for (long i = 0; i < tx_msg_len; i++)
        {
            txmsg.data[i] = (uint8_t)output.at(i);
        }
        // Indicate that the PassThruMsg array contains just a single message.
        num_msgs = 1;

        j2534_->PassThruWriteMsgs(chan_id_, &txmsg, &num_msgs, 100);
        // emit LOG_D("Data sent: " + parse_message_to_hex(output);

        output.remove(0, tx_msg_len);
        tx_msg_len = NextPassThruChunkLength(output);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SendPeriodicJ2534Data(QByteArray output, int timeout_arg)
{
    PassThruMsg txmsg;
    long tx_msg_len = NextPassThruChunkLength(output);

    while (tx_msg_len > 0)
    {
        emit logD("Start periodic messages with protocol: " + QString::number(protocol_), true, true);
        txmsg.protocol_id = protocol_;
        txmsg.rx_status = 0;
        txmsg.tx_flags = 0;
        txmsg.timestamp = 0;
        txmsg.data_size = tx_msg_len;
        txmsg.extra_data_index = 0;

        for (long i = 0; i < tx_msg_len; i++)
        {
            txmsg.data[i] = (uint8_t)output.at(i);
        }
        j2534_->PassThruStartPeriodicMsg(chan_id_, &txmsg, &msg_id_, timeout_arg);
        output.remove(0, tx_msg_len);
        tx_msg_len = NextPassThruChunkLength(output);
    }

    delay(10);

    emit logD("Start periodic message chanID: " + QString::number(chan_id_) +
                  " and msgID: " + QString::number(chan_id_),
              true, true);

    return kSerialSuccess;
}

int SerialPortActionsDirect::StopPeriodicJ2534Data()
{
    emit logD("Stop periodic message chanID: " + QString::number(chan_id_) + " and msgID: " + QString::number(chan_id_),
              true, true);
    j2534_->PassThruStopPeriodicMsg(chan_id_, msg_id_);
    delay(10);
    // j2534->PassThruReadMsgs(chanID, &rxmsg, &numRxMsg, timeout);

    return kSerialSuccess;
}

bool SerialPortActionsDirect::GetIsTxDone()
{
    return J2534TxDone();
}

QByteArray SerialPortActionsDirect::ReadJ2534Data(unsigned long timeout_arg)
{
    PassThruMsg rxmsg;
    unsigned long num_rx_msg;
    QByteArray received;

    J2534IoScope io(j2534_io_depth_); // block teardown while this read runs

    received.clear();

    rxmsg.data_size = 0;
    num_rx_msg = 1;

    if (j2534_->PassThruReadMsgs(chan_id_, &rxmsg, &num_rx_msg, timeout_arg))
    {
        goto exit;
    }

    if (num_rx_msg)
    {
        // dump_msg(&rxmsg);

        if (is_can_connection)
        {
            for (unsigned long i = 0; i < rxmsg.data_size; i++)
            {
                received.append(static_cast<char>(rxmsg.data[i]));
            }
        }
        else
        {
            if (rxmsg.rx_status & kTxDone)
            {
                rxmsg.data_size = 0;
                rxmsg.data[0] = 0x00;
                j2534_->PassThruReadMsgs(chan_id_, &rxmsg, &num_rx_msg, timeout_arg);
            }
            if (rxmsg.rx_status & kStartOfMessage)
            {
                j2534_->PassThruReadMsgs(chan_id_, &rxmsg, &num_rx_msg, timeout_arg);
            }
            if (rxmsg.rx_status & static_cast<unsigned long>(kRxMsgEndInd))
            {
            }
            for (unsigned long i = 0; i < rxmsg.data_size; i++)
            {
                received.append(static_cast<char>(rxmsg.data[i]));
            }
        }
    }
exit:
    return received;
}

int SerialPortActionsDirect::SetJ2534Ioctl(unsigned long parameter, int value)
{
    // Set timeouts etc.
    auto scp = std::to_array<SCONFIG>({{.parameter = parameter, .value = static_cast<unsigned long>(value)}});
    SConfigList scl = ConfigList(scp);
    if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
    {
        ReportJ2534Error();
        return kSerialError;
    }
    else
    {
        // emit LOG_D("Set timings OK";
    }

    return kSerialSuccess;
}

unsigned long SerialPortActionsDirect::ReadVbatt()
{
    if (!use_openport2_adapter || !j2534_)
    {
        // Adapter does not support reading voltage
        return kSerialSuccess;
    }
    J2534IoScope io(j2534_io_depth_); // block teardown while this read runs
    if (j2534_->PassThruIoctl(chan_id_, kJ2534ReadVbatt, nullptr, &v_batt))
    {
        ReportJ2534Error();
        return kSerialError;
    }
    // emit LOG_D("Batt: " + QString::number(vBatt / 1000.0) + " V", true, true);

    return v_batt;
}

void SerialPortActionsDirect::DumpMsg(PassThruMsg *msg)
{
    QByteArray datamsg;

    if (msg->rx_status & kStartOfMessage)
    {
        return; // skip
    }

    datamsg.clear();
    for (unsigned int i = 0; i < msg->data_size; i++)
    {
        datamsg.append(QString("%1 ").arg(msg->data[i], 2, 16, QLatin1Char('0')).toUtf8());
    }
    // emit LOG_D("Timestamp: " + msg->Timestamp << "msg length: " + msg->DataSize << "msg: " + datamsg;
}

bool SerialPortActionsDirect::GetSerialNum(char *serial_arg)
{
    struct
    {
        unsigned int length;
        std::array<unsigned char, 256> data;
    } outbuf{};

    outbuf.length = outbuf.data.size() - 1; // reserve one byte for the null terminator
    memcpy(serial_arg, outbuf.data.data(), outbuf.length);
    serial_arg[outbuf.length] = 0;
    return true;
}

int SerialPortActionsDirect::InitJ2534Connection()
{
    // If Linux, open serial port
    if (!OpenJ2534Transport())
    {
        return kSerialError;
    }

    // Init J2534 connection (in windows, load DLL etc.)
    if (!j2534_->Init())
    {
        emit logE("INIT: Can't load J2534 DLL.", true, true);
        return kSerialError;
    }
    else
    {
        emit logD("INIT: J2534 DLL loaded.", true, true);
    }

    dev_id_++;
    // Open J2534 connection
    if (j2534_->PassThruOpen(nullptr, &dev_id_))
    {
        CloseJ2534Transport();
        ReportJ2534Error();
        return kSerialError;
    }
    else
    {
        LogJ2534Opened();
    }

    // Get J2534 adapter and driver version numbers.
    // PassThruReadVersion takes no length, so these are read back through
    // bytes::fromFixedBufferDroppingLast, which stops at the buffer's end
    // whether or not the driver left a terminator.
    std::array<char, J2534::kVersionBufferSize> str_api_version{};
    std::array<char, J2534::kVersionBufferSize> str_dll_version{};
    std::array<char, J2534::kVersionBufferSize> str_firmware_version{};
    std::array<char, J2534::kVersionBufferSize> str_serial{};

    if (j2534_->PassThruReadVersion(str_api_version.data(), str_dll_version.data(), str_firmware_version.data(),
                                    dev_id_))
    {
        ReportJ2534Error();
        return kSerialError;
    }

    if (!GetSerialNum(str_serial.data()))
    {
        ReportJ2534Error();
        return kSerialError;
    }

    const std::string_view api_version = bytes::FromFixedBufferDroppingLast(str_api_version);
    const std::string_view dll_version = bytes::FromFixedBufferDroppingLast(str_dll_version);
    const std::string_view firmware_version = bytes::FromFixedBufferDroppingLast(str_firmware_version);
    const std::string_view serial_number = bytes::FromFixedBufferDroppingLast(str_serial);

    emit logD("J2534 API Version: " + QString::fromUtf8(api_version), true, true);
    emit logD("J2534 DLL Version: " + QString::fromUtf8(dll_version), true, true);
    emit logD("Device Firmware Version: " + QString::fromUtf8(firmware_version), true, true);
    const QByteArray serial_bytes =
        QByteArray::fromRawData(serial_number.data(), static_cast<qsizetype>(serial_number.size()));
    emit logD("Device Serial Number: " + ParseMessageToHex(serial_bytes), true, true);

    // Create J2534 to device connections
    if (is_iso15765_connection)
    {
        SetJ2534Can();
        SetJ2534CanTimings();
        SetJ2534CanFilters();
        emit logD("ISO15765 init ready", true, true);
    }
    else if (is_can_connection)
    {
        SetJ2534Can();
        SetJ2534CanTimings();
        SetJ2534CanFilters();
        emit logD("CAN init ready", true, true);
    }
    else
    {
        SetJ2534Iso9141();
        SetJ2534Iso9141Timings();
        SetJ2534Iso9141Filters();
        emit logD("K-Line init ready", true, true);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetJ2534Can()
{
    if (is_can_connection)
    {
        emit logD("Set CAN flags", true, true);
        protocol_ = kJ2534Can;
        if (is_29_bit_id)
        {
            flags_ = kJ2534Can29BitId;
        }
        else
        {
            flags_ = 0;
        }
    }
    else if (is_iso15765_connection)
    {
        emit logD("Set iso15765 flags", true, true);
        protocol_ = kJ2534Iso15765;
        if (is_29_bit_id)
        {
            flags_ = kJ2534Can29BitId;
        }
        else
        {
            flags_ = 0;
        }
    }
    // Denso DST-i hack
    if (j2534_is_denso_dsti_ && protocol_ == kJ2534Iso15765)
    {
        flags_ = 0;
    }
    baudrate_ = can_speed.toUInt();
    // use ISO9141_NO_CHECKSUM to disable checksumming on both tx and rx messages
    if (j2534_->PassThruConnect(dev_id_, protocol_, flags_, baudrate_, &chan_id_))
    {
        ReportJ2534Error();
        return kSerialError;
    }
    else
    {
        AdoptJ2534ChannelId();
        // emit LOG_D("Connected: " + devID << protocol << baudrate << chanID;
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::UnsetJ2534Can()
{
    if (j2534_->PassThruDisconnect(dev_id_))
    {
        ReportJ2534Error();
        return kSerialError;
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetJ2534CanTimings()
{
    // Set timeouts etc.
    if (is_can_connection)
    {
        emit logD("Set CAN timings", true, true);
    }
    else if (is_iso15765_connection)
    {
        emit logD("Set iso15765 timings", true, true);
    }
    auto scp = std::to_array<SCONFIG>({{.parameter = kJ2534Loopback, .value = 0}});
    SConfigList scl = ConfigList(scp);
    if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
    {
        ReportJ2534Error();
        return kSerialError;
    }
    else
    {
        // emit LOG_D("Set timings OK";
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetJ2534CanFilters()
{
    // now setup the filter(s)
    PassThruMsg txmsg;
    PassThruMsg msg_mask, msg_pattern, msg_flow;
    unsigned long msg_id;

    j2534_->PassThruIoctl(chan_id_, kJ2534ClearMsgFilters, nullptr, nullptr);

    if (is_can_connection)
    {
        emit logD("Set CAN filters", true, true);
        txmsg.protocol_id = protocol_;
        txmsg.rx_status = 0;
        txmsg.tx_flags = kJ2534Can29BitId;
        txmsg.timestamp = 0;
        txmsg.data_size = 4;
        txmsg.extra_data_index = 0;

        msg_mask = msg_pattern = txmsg;
        memset(msg_mask.data, 0xFF, txmsg.data_size);
        memset(msg_pattern.data, 0xFF, txmsg.data_size);

        bytes::WriteU32Be(msg_pattern.data, 0, can_destination_address);

        if (j2534_->PassThruStartMsgFilter(chan_id_, kJ2534PassFilter, &msg_mask, &msg_pattern, nullptr, &msg_id))
        {
            ReportJ2534Error();
            return kSerialError;
        }
    }
    else if (is_iso15765_connection)
    {
        emit logD("Set iso15765 filters", true, true);
        txmsg.protocol_id = protocol_;
        txmsg.rx_status = 0;
        txmsg.tx_flags = kIso15765FramePad;
        txmsg.timestamp = 0;
        txmsg.data_size = 4;
        txmsg.extra_data_index = 0;
        msg_mask = msg_pattern = msg_flow = txmsg;
        memset(msg_mask.data, 0xFF, txmsg.data_size);
        memset(msg_pattern.data, 0xFF, txmsg.data_size);
        memset(msg_flow.data, 0xFF, txmsg.data_size);

        bytes::WriteU32Be(msg_pattern.data, 0, iso15765_destination_address);
        bytes::WriteU32Be(msg_flow.data, 0, iso15765_source_address);

        if (j2534_->PassThruStartMsgFilter(chan_id_, kJ2534FlowControlFilter, &msg_mask, &msg_pattern, &msg_flow,
                                           &msg_id))
        {
            ReportJ2534Error();
            return kSerialError;
        }
    }
    else
    {
        return kSerialError;
    }

    if (is_can_connection)
    {
        emit logD("CAN filters OK", true, true);
    }
    else if (is_iso15765_connection)
    {
        emit logD("ISO15765 filters OK", true, true);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetJ2534Iso9141()
{
    baudrate_ = serial_port_baudrate.toUInt();

    if (is_iso14230_connection)
    {
        protocol_ = kJ2534Iso14230;
        flags_ = kIso9141NoChecksum | kCanIdBoth;
    }
    else
    {
        protocol_ = kJ2534Iso9141;
        flags_ = kIso9141NoChecksum;
    }

    emit logD("Protocol: " + QString::number(protocol_), true, true);

    if (j2534_is_denso_dsti_)
    {
        switch (protocol_)
        {
        case kJ2534Iso9141:
            // DST-i does not work well with ISO9141
            // sometimes it reads by 1 byte
            // Denso DST-i specific protocol, in fact ISO9141
            protocol_ = kJ2534DstiIso9141;
            flags_ = kIso9141NoChecksum;
            baudrate_ = 10400;
            break;
        case kJ2534Iso14230:
            flags_ = kJ2534Iso9141KLineOnly;
            break;
        default:
            break;
        }
    }

    // use ISO9141_NO_CHECKSUM to disable checksumming on both tx and rx messages
    if (j2534_->PassThruConnect(dev_id_, protocol_, flags_, baudrate_, &chan_id_))
    {
        ReportJ2534Error();
        return kSerialError;
    }
    else
    {
        AdoptJ2534ChannelId();
        emit logD("Connected: DevID " + QString::number(dev_id_) + ", protocol " + QString::number(protocol_) +
                      ", baudrate " + QString::number(baudrate_) + ", chanID " + QString::number(chan_id_),
                  true, true);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetJ2534Iso9141Timings()
{
    if (j2534_is_denso_dsti_)
    {
        auto scp_dsti_is_o14230 = std::to_array<SCONFIG>({{.parameter = kJ2534Loopback, .value = 0},
                                                          {.parameter = kJ2534P1Max, .value = 0xa},
                                                          {.parameter = kJ2534P3Min, .value = 0x14},
                                                          {.parameter = kJ2534P4Min, .value = 0},
                                                          {.parameter = kJ2534DataRate, .value = 4800}});
        auto scp_dsti_dsti_is_o9141 = std::to_array<SCONFIG>({{.parameter = kJ2534DataRate, .value = 4800},
                                                              {.parameter = kJ2534Loopback, .value = 0},
                                                              {.parameter = kJ2534P1Min, .value = 0},
                                                              {.parameter = kJ2534P1Max, .value = 4},
                                                              {.parameter = kJ2534P2Min, .value = 4},
                                                              {.parameter = kJ2534P2Max, .value = 0x14},
                                                              {.parameter = kJ2534P3Min, .value = 0x14},
                                                              {.parameter = kJ2534P3Max, .value = 10000},
                                                              {.parameter = kJ2534P4Min, .value = 0},
                                                              {.parameter = kJ2534P4Max, .value = 0x14}});

        ClearTxBuffer();
        ClearRxBuffer();

        // Zero-initialised, not left indeterminate: `protocol` is not
        // necessarily one of the two cases below.
        SConfigList scl{};
        switch (protocol_)
        {
        case kJ2534Iso14230:
            scl = ConfigList(scp_dsti_is_o14230);
            break;
        case kJ2534DstiIso9141:
            scl = ConfigList(scp_dsti_dsti_is_o9141);
            break;
        default:
            break;
        }

        if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
        {
            ReportJ2534Error();
            return kSerialError;
        }
    }
    else
    {
        // Set timeouts etc.
        unsigned long parity = kJ2534NoParity;
        if (serial_port_parity == QSerialPort::OddParity)
        {
            parity = kJ2534OddParity;
        }
        else if (serial_port_parity == QSerialPort::EvenParity)
        {
            parity = kJ2534EvenParity;
        }
        auto scp = std::to_array<SCONFIG>({{.parameter = kJ2534Loopback, .value = 0},
                                           {.parameter = kJ2534P1Max, .value = 1},
                                           {.parameter = kJ2534P3Min, .value = 0},
                                           {.parameter = kJ2534P4Min, .value = 0},
                                           {.parameter = kJ2534Parity, .value = parity},
                                           {.parameter = kJ2534Tinil, .value = 25}});
        SConfigList scl = ConfigList(scp);
        if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
        {
            ReportJ2534Error();
            return kSerialError;
        }
        else
        {
            // emit LOG_D("Set timings OK";
        }
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::SetJ2534Iso9141Filters()
{
    // now setup the filter(s)
    PassThruMsg txmsg;
    PassThruMsg msg_mask, msg_pattern;
    unsigned long msg_id;

    // simply create a "pass all" filter so that we can see
    // everything unfiltered in the raw stream

    txmsg.protocol_id = protocol_;
    txmsg.rx_status = 0;
    txmsg.tx_flags = 0;
    txmsg.timestamp = 0;
    txmsg.data_size = 4;
    txmsg.extra_data_index = 0;
    msg_mask = msg_pattern = txmsg;
    memset(msg_mask.data, 0, txmsg.data_size);    // mask the first 4 byte to 0
    memset(msg_pattern.data, 0, txmsg.data_size); // match it with 0 (i.e. pass everything)
    if (j2534_->PassThruStartMsgFilter(chan_id_, kJ2534PassFilter, &msg_mask, &msg_pattern, nullptr, &msg_id))
    {
        ReportJ2534Error();
        return kSerialError;
    }
    else
    {
        // emit LOG_D("Set filters OK";
    }
    // j2534->PassThruSetProgrammingVoltage(devID, J1962_PIN_9, 5000);

    return kSerialSuccess;
}

void SerialPortActionsDirect::ReportJ2534Error()
{
    std::array<char, 512> err{};
    j2534_->PassThruGetLastError(err.data());
    emit logD("J2534 error: " + (QString)err.data(), true, true);
}

void SerialPortActionsDirect::handleError(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::NoError)
    {
        emit logD("Error: " + QString::number(error), true, true);
    }

    switch (error)
    {
    case QSerialPort::DeviceNotFoundError:
    case QSerialPort::OpenError:
    case QSerialPort::NotOpenError:
    case QSerialPort::WriteError:
    case QSerialPort::ReadError:
    case QSerialPort::ResourceError:
    case QSerialPort::UnknownError:
        ResetConnection();
        break;
    case QSerialPort::TimeoutError:
        // The read paths now poll via serial->waitForReadyRead(1) instead of
        // pumping the event loop; QSerialPort emits this error whenever such a
        // wait elapses with no new bytes, which is the ordinary case on every
        // polling tick, not a device/link failure. Previously this error was
        // unreachable (nothing called a wait-with-timeout API), so it could
        // safely reset the connection; now that would tear it down on routine
        // polling latency. Leave it a no-op, like the other non-fatal errors.
    // NoError, PermissionError, UnsupportedOperationError and the Parity/Framing/BreakCondition
    // errors are non-fatal and deliberately left alone.
    default:
        break;
    }
}

void SerialPortActionsDirect::accurateDelay(double timeout_arg)
{
    double seconds = timeout_arg / 1000.0;
    auto spin_start = std::chrono::high_resolution_clock::now();
    while (static_cast<double>((std::chrono::high_resolution_clock::now() - spin_start).count()) / 1e9 < seconds)
    {
        ;
    }
}

void SerialPortActionsDirect::fastDelay(int timeout_arg)
{
    QThread::msleep(timeout_arg);
}

void SerialPortActionsDirect::delay(int timeout_arg)
{
    QThread::msleep(timeout_arg);
}

QString SerialPortActionsDirect::ParseMessageToHex(const QByteArray& received)
{
    QByteArray msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}
