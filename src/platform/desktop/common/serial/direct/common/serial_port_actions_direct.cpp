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
long nextPassThruChunkLength(const QByteArray& remaining)
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
template <std::size_t N> SConfigList configList(std::array<SCONFIG, N>& params)
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
    connect_j2534_logs();
}

SerialPortActionsDirect::~SerialPortActionsDirect()
{
    delete j2534_;
    delete serial_;
}

bool SerialPortActionsDirect::is_serial_port_open()
{
    if (!serial_->isOpen())
    {
        if (!j2534_init_ok_)
        {
            return false;
        }
        else
        {
            return j2534_ && j2534_->is_serial_port_open();
        }
    }

    return serial_->isOpen();
}

bool SerialPortActionsDirect::set_kline_timings(uint32_t parameter, int value)
{
    p1_max_ms = value;
    return kSerialSuccess;
}

int SerialPortActionsDirect::change_port_speed(QString port_speed)
{
    serial_port_baudrate = port_speed;
    baudrate_ = port_speed.toInt();

    emit LOG_D("Changing baudrate, checking if port is open...", true, true);
    if (is_serial_port_open())
    {
        emit LOG_D("Port is open, checking adapter type...", true, true);
        if (!use_openport2_adapter)
        {
            emit LOG_D("Adapter type is generic OBD2...", true, true);

            if (serial_->setBaudRate(static_cast<qint32>(serial_port_baudrate.toDouble())))
            {
                delay(50);
                emit LOG_D("Baudrate set to " + port_speed + " OK", true, true);
                return kSerialSuccess;
            }
            else
            {
                emit LOG_E("ERROR setting baudrate!", true, true);
                return kSerialError;
            }
        }
        else
        {
            emit LOG_D("Adapter type is J2534...", true, true);

            auto scp = std::to_array<SCONFIG>({{.parameter = kJ2534DataRate, .value = baudrate_}});
            SConfigList scl = configList(scp);
            if (!j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
            {
                emit LOG_D("Baudrate set to " + port_speed + " OK", true, true);
                delay(50);
                return kSerialSuccess;
            }
            else
            {
                reportJ2534Error();
                return kSerialError;
            }
        }
    }

    return kSerialError;
}

QByteArray SerialPortActionsDirect::five_baud_init(QByteArray output)
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
            reportJ2534Error();
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
        accurate_delay(350);
        // Set break to set seril line low
        serial_->setBreakEnabled(true);
        // Set timeout to 200ms to generate 200ms low pulse
        accurate_delay(200);
        // Unset break to set seril line high
        serial_->setBreakEnabled(false);
        // Set timeout to 400ms to generate 400ms high pulse
        accurate_delay(400);
        serial_->setBreakEnabled(true);
        accurate_delay(400);
        serial_->setBreakEnabled(false);
        accurate_delay(400);
        serial_->setBreakEnabled(true);
        accurate_delay(400);
        serial_->setBreakEnabled(false);
        accurate_delay(400);
        // Set timeout to 400ms to generate 400ms high pulse before init data is sent

        // Send init data
        write_serial_data_echo_check(output);
        response = read_serial_obd_data(40);
        // emit LOG_D("Read response", true, true);
        response = read_serial_obd_data(200);
        // emit LOG_D("Five baud init response: " + parse_message_to_hex(response), true, true);
        if ((uint8_t)response.at(1) == 0x08 && (uint8_t)response.at(2) == 0x08)
        {
            // delay(30);
            output.clear();
            output.append(static_cast<char>(~static_cast<uint8_t>(response.at(2))));
            write_serial_data_echo_check(output);
            response.append(read_serial_obd_data(200));
        }
    }

    return response;
}

int SerialPortActionsDirect::fast_init(QByteArray output)
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
            output = append_ssm_header(output);
        }
        else if (add_iso9141_header)
        {
            output = append_iso9141_header(output);
        }
        else if (add_iso14230_header)
        {
            output = append_iso14230_header(output);
        }

        for (int i = 0; i < output.length(); i++)
        {
            input_msg.data[i] = (uint8_t)output.at(i);
        }
        input_msg.data_size = output.length();

        /* Set timeout to 350ms before init */
        accurate_delay(350);

        result = j2534_->PassThruIoctl(chan_id_, kJ2534FastInit, &input_msg, &output_msg);
        if (result)
        {
            reportJ2534Error();
            return kSerialError;
        }
    }
    else
    {
        // Set timeout to 350ms before init
        accurate_delay(350);
        // Set break to set seril line low
        serial_->setBreakEnabled(true);
        // Set timeout to 25ms to generate 25ms low pulse
        accurate_delay(23.7);
        // Unset break to set seril line high
        serial_->setBreakEnabled(false);
        // Set timeout to 25ms to generate 25ms high pulse before init data is sent
        accurate_delay(23.8);
        // Send init data
        write_serial_data_echo_check(output);
        received = read_serial_data(10);
        // emit LOG_D("Fast init response: " + parse_message_to_hex(received), true, true);
        delay(100);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::clear_rx_buffer()
{
    if (use_openport2_adapter)
    {
        unsigned long status;

        status = j2534_->PassThruIoctl(chan_id_, kJ2534ClearRxBuffer, nullptr, nullptr);
        if (status)
        {
            reportJ2534Error();
            return kSerialError;
        }
        else
        {
            emit LOG_D("RX BUFFER EMPTY", true, true);
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

int SerialPortActionsDirect::clear_tx_buffer()
{
    if (use_openport2_adapter)
    {
        unsigned long status;

        status = j2534_->PassThruIoctl(chan_id_, kJ2534ClearTxBuffer, nullptr, nullptr);
        if (status)
        {
            reportJ2534Error();
            return kSerialError;
        }
        else
        {
            emit LOG_D("TX BUFFER EMPTY", true, true);
        }
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_lec_lines(int lec1_state, int lec2_state)
{
    line_end_check_1_toggled(lec1_state);
    line_end_check_2_toggled(lec2_state);

    return kSerialSuccess;
}

int SerialPortActionsDirect::pulse_lec_1_line(int timeout_arg)
{
    line_end_check_1_toggled(request_to_send_enabled);
    accurate_delay(timeout_arg);
    line_end_check_1_toggled(request_to_send_disabled);
    // delay(timeout);

    // read_serial_data(100, 50);

    return kSerialSuccess;
}

int SerialPortActionsDirect::pulse_lec_2_line(int timeout_arg)
{
    line_end_check_2_toggled(data_terminal_enabled);
    accurate_delay(timeout_arg);
    line_end_check_2_toggled(data_terminal_disabled);
    // delay(timeout);

    // read_serial_data(100, 50);

    return kSerialSuccess;
}

int SerialPortActionsDirect::line_end_check_1_toggled(int state)
{
    if (state == request_to_send_enabled)
    {
        if (use_openport2_adapter)
        {
            j2534_->PassThruSetProgrammingVoltage(dev_id_, kJ2534J1962Pin11, 12000);
            settle_after_programming_voltage();
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

int SerialPortActionsDirect::line_end_check_2_toggled(int state)
{
    if (state == data_terminal_enabled)
    {
        if (use_openport2_adapter)
        {
            j2534_->PassThruSetProgrammingVoltage(dev_id_, kJ2534J1962Pin9, 12000);
            settle_after_programming_voltage();
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

QStringList SerialPortActionsDirect::check_serial_ports()
{
    const auto serial_ports_info = QSerialPortInfo::availablePorts();
    QStringList serial_ports;
    // QString j2534DllName = "j2534.dll";

    serial_port_available = false;

    for (const QSerialPortInfo& serial_port_info : serial_ports_info)
    {
        serial_ports.append(serial_port_info.portName() + " - " + serial_port_info.description());
        emit LOG_D("Serial port name: " + serial_port_info.portName() + " " + serial_port_info.description(), true,
                   true);
    }
    std::sort(serial_ports.begin(), serial_ports.end(), std::less<QString>());

    append_j2534_interfaces(serial_ports);

    return serial_ports;
}

QString SerialPortActionsDirect::open_serial_port()
{
    emit LOG_D("Serial port = " + serial_port_list.join(", "), true, true);
    // QString serial_port_text = serial_port_list.at(1);
    const ResolvedPort resolved = resolve_port(serial_port_list.at(0));
    serial_port = resolved.port;
    emit LOG_D("Interface: " + serial_port, true, true);

    if (!serial_port.isEmpty() && resolved.is_j2534)
    {
        reset_connection();
        // close_serial_port();
        // use_openport2_adapter = true;

        j2534_is_denso_dsti_ = serial_port.contains("DST-i");

        // QMap<QString, QString> user_j2534_drivers; // Local drivers in software folder
        select_j2534_dll();
        long result;

        emit LOG_D("Testing j2534 interface, please wait...", true, true);
        result = init_j2534_connection();

        if (result == kSerialSuccess)
        {
            emit LOG_D("J2534: Interface opened succesfully!", true, true);
            use_openport2_adapter = true;
            j2534_init_ok_ = true;
            j2534_->j2534_init_ok = true;
            opened_serial_port = serial_port;
        }
        else
        {
            emit LOG_D("J2534: Failed to open interface!", true, true);
            use_openport2_adapter = false;
            reset_connection();
        }
    }
    if (!use_openport2_adapter && opened_serial_port != serial_port)
    {
        emit LOG_D("Testing serial interface, please wait...", true, true);
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
                        SLOT(handle_error(QSerialPort::SerialPortError)));

                emit LOG_D("Serial port '" + serial_port + "' is open at baudrate " + serial_port_baudrate, true, true);
                return opened_serial_port;
            }
            else
            {
                emit LOG_E("Couldn't open serial port '" + serial_port + "'", true, true);
                return {};
            }
        }
        else
        {
            emit LOG_D("Serial port '" + serial_port + "' is already opened", true, true);
            return opened_serial_port;
        }
    }

    return opened_serial_port;
}

void SerialPortActionsDirect::reset_connection()
{
    close_j2534_serial_port();
    close_serial_port();
    delay(250);
}

void SerialPortActionsDirect::close_serial_port()
{
    if (serial_->isOpen())
    {
        serial_->close();
        opened_serial_port.clear();
    }
}

void SerialPortActionsDirect::close_j2534_serial_port()
{
    if (j2534_io_depth_ > 0)
    {
        // A J2534 read is in-flight (its read paths pumped the event loop and we
        // were re-entered). Freeing j2534 now would pull the object out from under
        // that in-flight read. Skip the teardown; it can run once the read returns.
        emit LOG_D("Skipping J2534 reset: a read is in-flight (reentrancy guard)", true, true);
        return;
    }
    if (j2534_->is_serial_port_open())
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
            emit LOG_D("J2534 interface disconnect failed!", true, true);
        }
        else
        {
            emit LOG_D("J2534 interface disconnected succesfully!", true, true);
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
            emit LOG_D("J2534 interface close failed!", true, true);
        }
        else
        {
            emit LOG_D("J2534 interface closed succesfully!", true, true);
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
    j2534_->getDllName(dll_name.data());
    delete j2534_;
    // Null the pointer across the delay(): delay() pumps the event loop, so a
    // reentrant read can run here. A guarded read sees null (safe) instead of a
    // dangling pointer (use-after-free).
    j2534_ = nullptr;
    delay(100);
    j2534_ = new J2534();
    j2534_->setDllName(dll_name.data());
    connect_j2534_logs();
}

QByteArray SerialPortActionsDirect::set_error()
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

QByteArray SerialPortActionsDirect::read_serial_obd_data(uint16_t timeout_arg)
{
    QByteArray received;

    if (is_serial_port_open())
    {
        if (use_openport2_adapter)
        {
            received = read_j2534_data(timeout_arg);
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

QByteArray SerialPortActionsDirect::read_serial_data(uint16_t timeout_arg)
{
    QByteArray received;
    QByteArray req_bytes;
    uint32_t msglen = 0;

    if (is_serial_port_open())
    {
        if (use_openport2_adapter)
        {
            received = read_j2534_data(timeout_arg);
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
                    msglen = bytes::readU16Be(bytes::view(received), 2) + 1; // +1 for checksum
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
            received.insert(0, set_error());
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

QByteArray SerialPortActionsDirect::write_serial_data(QByteArray output)
{
    QByteArray received;
    QByteArray msg;

    msg.append('\0');

    if (is_serial_port_open())
    {
        if (add_ssm_header)
        {
            output = append_ssm_header(output);
        }
        else if (add_iso9141_header)
        {
            output = append_iso9141_header(output);
        }
        else if (add_iso14230_header)
        {
            output = append_iso14230_header(output);
        }

        if (use_openport2_adapter)
        {
            write_j2534_data(output);
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

QByteArray SerialPortActionsDirect::write_serial_data_echo_check(QByteArray output)
{
    QByteArray received;
    QByteArray msg;

    msg.append('\0');

    if (is_serial_port_open())
    {
        if (add_ssm_header)
        {
            output = append_ssm_header(output);
        }
        else if (add_iso9141_header)
        {
            output = append_iso9141_header(output);
        }
        else if (add_iso14230_header)
        {
            output = append_iso14230_header(output);
        }

        if (use_openport2_adapter)
        {
            write_j2534_data(output);
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
            emit LOG_D("Write serial data echo read failed!", true, true);
        }

        received.clear();
        return {};
    }
    return {};
}

QByteArray SerialPortActionsDirect::append_ssm_header(QByteArray output)
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

QByteArray SerialPortActionsDirect::append_iso9141_header(QByteArray output)
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

QByteArray SerialPortActionsDirect::append_iso14230_header(QByteArray output)
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

int SerialPortActionsDirect::write_j2534_data(QByteArray output)
{
    PassThruMsg txmsg;
    unsigned long num_msgs;
    long tx_msg_len = nextPassThruChunkLength(output);

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
        tx_msg_len = nextPassThruChunkLength(output);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::send_periodic_j2534_data(QByteArray output, int timeout_arg)
{
    PassThruMsg txmsg;
    long tx_msg_len = nextPassThruChunkLength(output);

    while (tx_msg_len > 0)
    {
        emit LOG_D("Start periodic messages with protocol: " + QString::number(protocol_), true, true);
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
        tx_msg_len = nextPassThruChunkLength(output);
    }

    delay(10);

    emit LOG_D("Start periodic message chanID: " + QString::number(chan_id_) +
                   " and msgID: " + QString::number(chan_id_),
               true, true);

    return kSerialSuccess;
}

int SerialPortActionsDirect::stop_periodic_j2534_data()
{
    emit LOG_D("Stop periodic message chanID: " + QString::number(chan_id_) +
                   " and msgID: " + QString::number(chan_id_),
               true, true);
    j2534_->PassThruStopPeriodicMsg(chan_id_, msg_id_);
    delay(10);
    // j2534->PassThruReadMsgs(chanID, &rxmsg, &numRxMsg, timeout);

    return kSerialSuccess;
}

bool SerialPortActionsDirect::get_is_tx_done()
{
    return j2534_tx_done();
}

QByteArray SerialPortActionsDirect::read_j2534_data(unsigned long timeout_arg)
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

int SerialPortActionsDirect::set_j2534_ioctl(unsigned long parameter, int value)
{
    // Set timeouts etc.
    auto scp = std::to_array<SCONFIG>({{.parameter = parameter, .value = static_cast<unsigned long>(value)}});
    SConfigList scl = configList(scp);
    if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
    {
        reportJ2534Error();
        return kSerialError;
    }
    else
    {
        // emit LOG_D("Set timings OK";
    }

    return kSerialSuccess;
}

unsigned long SerialPortActionsDirect::read_vbatt()
{
    if (!use_openport2_adapter || !j2534_)
    {
        // Adapter does not support reading voltage
        return kSerialSuccess;
    }
    J2534IoScope io(j2534_io_depth_); // block teardown while this read runs
    if (j2534_->PassThruIoctl(chan_id_, kJ2534ReadVbatt, nullptr, &v_batt))
    {
        reportJ2534Error();
        return kSerialError;
    }
    // emit LOG_D("Batt: " + QString::number(vBatt / 1000.0) + " V", true, true);

    return v_batt;
}

void SerialPortActionsDirect::dump_msg(PassThruMsg *msg)
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

bool SerialPortActionsDirect::get_serial_num(char *serial_arg)
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

int SerialPortActionsDirect::init_j2534_connection()
{
    // If Linux, open serial port
    if (!open_j2534_transport())
    {
        return kSerialError;
    }

    // Init J2534 connection (in windows, load DLL etc.)
    if (!j2534_->init())
    {
        emit LOG_E("INIT: Can't load J2534 DLL.", true, true);
        return kSerialError;
    }
    else
    {
        emit LOG_D("INIT: J2534 DLL loaded.", true, true);
    }

    dev_id_++;
    // Open J2534 connection
    if (j2534_->PassThruOpen(nullptr, &dev_id_))
    {
        close_j2534_transport();
        reportJ2534Error();
        return kSerialError;
    }
    else
    {
        log_j2534_opened();
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
        reportJ2534Error();
        return kSerialError;
    }

    if (!get_serial_num(str_serial.data()))
    {
        reportJ2534Error();
        return kSerialError;
    }

    const std::string_view api_version = bytes::fromFixedBufferDroppingLast(str_api_version);
    const std::string_view dll_version = bytes::fromFixedBufferDroppingLast(str_dll_version);
    const std::string_view firmware_version = bytes::fromFixedBufferDroppingLast(str_firmware_version);
    const std::string_view serial_number = bytes::fromFixedBufferDroppingLast(str_serial);

    emit LOG_D("J2534 API Version: " + QString::fromUtf8(api_version), true, true);
    emit LOG_D("J2534 DLL Version: " + QString::fromUtf8(dll_version), true, true);
    emit LOG_D("Device Firmware Version: " + QString::fromUtf8(firmware_version), true, true);
    const QByteArray serial_bytes =
        QByteArray::fromRawData(serial_number.data(), static_cast<qsizetype>(serial_number.size()));
    emit LOG_D("Device Serial Number: " + parse_message_to_hex(serial_bytes), true, true);

    // Create J2534 to device connections
    if (is_iso15765_connection)
    {
        set_j2534_can();
        set_j2534_can_timings();
        set_j2534_can_filters();
        emit LOG_D("ISO15765 init ready", true, true);
    }
    else if (is_can_connection)
    {
        set_j2534_can();
        set_j2534_can_timings();
        set_j2534_can_filters();
        emit LOG_D("CAN init ready", true, true);
    }
    else
    {
        set_j2534_iso9141();
        set_j2534_iso9141_timings();
        set_j2534_iso9141_filters();
        emit LOG_D("K-Line init ready", true, true);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_j2534_can()
{
    if (is_can_connection)
    {
        emit LOG_D("Set CAN flags", true, true);
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
        emit LOG_D("Set iso15765 flags", true, true);
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
        reportJ2534Error();
        return kSerialError;
    }
    else
    {
        adopt_j2534_channel_id();
        // emit LOG_D("Connected: " + devID << protocol << baudrate << chanID;
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::unset_j2534_can()
{
    if (j2534_->PassThruDisconnect(dev_id_))
    {
        reportJ2534Error();
        return kSerialError;
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_j2534_can_timings()
{
    // Set timeouts etc.
    if (is_can_connection)
    {
        emit LOG_D("Set CAN timings", true, true);
    }
    else if (is_iso15765_connection)
    {
        emit LOG_D("Set iso15765 timings", true, true);
    }
    auto scp = std::to_array<SCONFIG>({{.parameter = kJ2534Loopback, .value = 0}});
    SConfigList scl = configList(scp);
    if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
    {
        reportJ2534Error();
        return kSerialError;
    }
    else
    {
        // emit LOG_D("Set timings OK";
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_j2534_can_filters()
{
    // now setup the filter(s)
    PassThruMsg txmsg;
    PassThruMsg msg_mask, msg_pattern, msg_flow;
    unsigned long msg_id;

    j2534_->PassThruIoctl(chan_id_, kJ2534ClearMsgFilters, nullptr, nullptr);

    if (is_can_connection)
    {
        emit LOG_D("Set CAN filters", true, true);
        txmsg.protocol_id = protocol_;
        txmsg.rx_status = 0;
        txmsg.tx_flags = kJ2534Can29BitId;
        txmsg.timestamp = 0;
        txmsg.data_size = 4;
        txmsg.extra_data_index = 0;

        msg_mask = msg_pattern = txmsg;
        memset(msg_mask.data, 0xFF, txmsg.data_size);
        memset(msg_pattern.data, 0xFF, txmsg.data_size);

        bytes::writeU32Be(msg_pattern.data, 0, can_destination_address);

        if (j2534_->PassThruStartMsgFilter(chan_id_, kJ2534PassFilter, &msg_mask, &msg_pattern, nullptr, &msg_id))
        {
            reportJ2534Error();
            return kSerialError;
        }
    }
    else if (is_iso15765_connection)
    {
        emit LOG_D("Set iso15765 filters", true, true);
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

        bytes::writeU32Be(msg_pattern.data, 0, iso15765_destination_address);
        bytes::writeU32Be(msg_flow.data, 0, iso15765_source_address);

        if (j2534_->PassThruStartMsgFilter(chan_id_, kJ2534FlowControlFilter, &msg_mask, &msg_pattern, &msg_flow,
                                           &msg_id))
        {
            reportJ2534Error();
            return kSerialError;
        }
    }
    else
    {
        return kSerialError;
    }

    if (is_can_connection)
    {
        emit LOG_D("CAN filters OK", true, true);
    }
    else if (is_iso15765_connection)
    {
        emit LOG_D("ISO15765 filters OK", true, true);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_j2534_iso9141()
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

    emit LOG_D("Protocol: " + QString::number(protocol_), true, true);

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
        reportJ2534Error();
        return kSerialError;
    }
    else
    {
        adopt_j2534_channel_id();
        emit LOG_D("Connected: DevID " + QString::number(dev_id_) + ", protocol " + QString::number(protocol_) +
                       ", baudrate " + QString::number(baudrate_) + ", chanID " + QString::number(chan_id_),
                   true, true);
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_j2534_iso9141_timings()
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

        clear_tx_buffer();
        clear_rx_buffer();

        // Zero-initialised, not left indeterminate: `protocol` is not
        // necessarily one of the two cases below.
        SConfigList scl{};
        switch (protocol_)
        {
        case kJ2534Iso14230:
            scl = configList(scp_dsti_is_o14230);
            break;
        case kJ2534DstiIso9141:
            scl = configList(scp_dsti_dsti_is_o9141);
            break;
        default:
            break;
        }

        if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
        {
            reportJ2534Error();
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
        SConfigList scl = configList(scp);
        if (j2534_->PassThruIoctl(chan_id_, kJ2534SetConfig, &scl, nullptr))
        {
            reportJ2534Error();
            return kSerialError;
        }
        else
        {
            // emit LOG_D("Set timings OK";
        }
    }

    return kSerialSuccess;
}

int SerialPortActionsDirect::set_j2534_iso9141_filters()
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
        reportJ2534Error();
        return kSerialError;
    }
    else
    {
        // emit LOG_D("Set filters OK";
    }
    // j2534->PassThruSetProgrammingVoltage(devID, J1962_PIN_9, 5000);

    return kSerialSuccess;
}

void SerialPortActionsDirect::reportJ2534Error()
{
    std::array<char, 512> err{};
    j2534_->PassThruGetLastError(err.data());
    emit LOG_D("J2534 error: " + (QString)err.data(), true, true);
}

void SerialPortActionsDirect::handle_error(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::NoError)
    {
        emit LOG_D("Error: " + QString::number(error), true, true);
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
        reset_connection();
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

void SerialPortActionsDirect::accurate_delay(double timeout_arg)
{
    double seconds = timeout_arg / 1000.0;
    auto spin_start = std::chrono::high_resolution_clock::now();
    while (static_cast<double>((std::chrono::high_resolution_clock::now() - spin_start).count()) / 1e9 < seconds)
    {
        ;
    }
}

void SerialPortActionsDirect::fast_delay(int timeout_arg)
{
    QThread::msleep(timeout_arg);
}

void SerialPortActionsDirect::delay(int timeout_arg)
{
    QThread::msleep(timeout_arg);
}

QString SerialPortActionsDirect::parse_message_to_hex(const QByteArray& received)
{
    QByteArray msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}
