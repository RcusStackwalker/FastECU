#include "src/platform/desktop/common/connection/adapter_connection.h"

#include <QSerialPort>

#include <cstdint>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::desktop::connection
{

LogTransport log_transport_from_text(const QString& text)
{
    if (text == "CAN")
    {
        return LogTransport::Can;
    }
    if (text == "iso15765")
    {
        return LogTransport::Iso15765;
    }
    if (text == "K-Line")
    {
        return LogTransport::KLine;
    }
    if (text == "SSM")
    {
        return LogTransport::Ssm;
    }
    return LogTransport::Other;
}

AdapterConnection::AdapterConnection(SerialPortActions& facade, QObject *parent) : QObject(parent), facade_(facade)
{
    connect(&facade_, &SerialPortActions::stateChanged, this, &AdapterConnection::stateChanged, Qt::DirectConnection);
}

QStringList AdapterConnection::available_ports()
{
    return facade_.check_serial_ports();
}

void AdapterConnection::set_initial_port(const QString& port, const QString& baud)
{
    facade_.set_serial_port_baudrate(baud);
    facade_.set_serial_port(port);
}

void AdapterConnection::select_port(const QString& port)
{
    facade_.set_serial_port_list(QStringList{port});
}

QString AdapterConnection::open()
{
    return facade_.open_serial_port();
}

QString AdapterConnection::opened_port()
{
    return facade_.get_openedSerialPort();
}

bool AdapterConnection::is_open()
{
    return facade_.is_serial_port_open();
}

void AdapterConnection::reset()
{
    facade_.reset_connection();
}

void AdapterConnection::apply_log_transport(LogTransport transport, bool ssm_protocol)
{
    facade_.set_is_can_connection(false);
    facade_.set_is_iso15765_connection(false);
    switch (transport)
    {
    case LogTransport::Can:
        facade_.set_is_can_connection(true);
        facade_.set_is_iso15765_connection(false);
        facade_.set_is_29_bit_id(false);
        facade_.set_can_speed("500000");
        break;
    case LogTransport::Iso15765:
        facade_.set_is_can_connection(false);
        facade_.set_is_iso15765_connection(true);
        facade_.set_is_29_bit_id(true);
        facade_.set_can_speed("500000");
        break;
    case LogTransport::KLine:
        if (ssm_protocol)
        {
            facade_.change_port_speed("4800");
        }
        break;
    case LogTransport::Ssm:
    case LogTransport::Other:
        break;
    }
    facade_.reset_connection();
}

void AdapterConnection::clear_link_flags()
{
    facade_.reset_connection();
    facade_.set_is_iso14230_connection(false);
    facade_.set_is_29_bit_id(false);
    facade_.set_add_iso14230_header(false);
    facade_.set_is_can_connection(false);
    facade_.set_is_iso15765_connection(false);
    facade_.set_serial_port_baudrate("4800");
}

void AdapterConnection::return_to_idle()
{
    facade_.reset_connection();
    facade_.set_serial_port_baudrate("4800");
    facade_.set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity));
}

void AdapterConnection::set_port_speed(int baud)
{
    facade_.change_port_speed(QString::number(baud));
}

std::optional<unsigned long> AdapterConnection::battery_millivolts()
{
    if (!facade_.get_use_openport2_adapter())
    {
        return std::nullopt;
    }
    return facade_.read_vbatt();
}

void AdapterConnection::wait_for_source()
{
    facade_.waitForSource();
}

SerialPortActions& AdapterConnection::facade()
{
    return facade_;
}

} // namespace fastecu::desktop::connection
