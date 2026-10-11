#include "src/platform/desktop/common/connection/adapter_connection.h"

#include <QSerialPort>

#include <cstdint>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::desktop::connection
{

LogTransport LogTransportFromText(const QString& text)
{
    if (text == "CAN")
    {
        return LogTransport::kCan;
    }
    if (text == "iso15765")
    {
        return LogTransport::kIso15765;
    }
    if (text == "K-Line")
    {
        return LogTransport::kKLine;
    }
    if (text == "SSM")
    {
        return LogTransport::kSsm;
    }
    return LogTransport::kOther;
}

AdapterConnection::AdapterConnection(SerialPortActions& facade, QObject *parent) : QObject(parent), facade_(facade)
{
    connect(&facade_, &SerialPortActions::stateChanged, this, &AdapterConnection::stateChanged, Qt::DirectConnection);
}

QStringList AdapterConnection::AvailablePorts()
{
    return facade_.CheckSerialPorts();
}

void AdapterConnection::SetInitialPort(const QString& port, const QString& baud)
{
    facade_.SetSerialPortBaudrate(baud);
    facade_.SetSerialPort(port);
}

void AdapterConnection::SelectPort(const QString& port)
{
    facade_.SetSerialPortList(QStringList{port});
}

QString AdapterConnection::Open()
{
    return facade_.OpenSerialPort();
}

QString AdapterConnection::OpenedPort()
{
    return facade_.GetOpenedSerialPort();
}

bool AdapterConnection::IsOpen()
{
    return facade_.IsSerialPortOpen();
}

void AdapterConnection::Reset()
{
    facade_.ResetConnection();
}

void AdapterConnection::ApplyLogTransport(LogTransport transport, bool ssm_protocol)
{
    facade_.SetIsCanConnection(false);
    facade_.SetIsIso15765Connection(false);
    switch (transport)
    {
    case LogTransport::kCan:
        facade_.SetIsCanConnection(true);
        facade_.SetIsIso15765Connection(false);
        facade_.SetIs29BitId(false);
        facade_.SetCanSpeed("500000");
        break;
    case LogTransport::kIso15765:
        facade_.SetIsCanConnection(false);
        facade_.SetIsIso15765Connection(true);
        facade_.SetIs29BitId(true);
        facade_.SetCanSpeed("500000");
        break;
    case LogTransport::kKLine:
        if (ssm_protocol)
        {
            facade_.ChangePortSpeed("4800");
        }
        break;
    case LogTransport::kSsm:
    case LogTransport::kOther:
        break;
    }
    facade_.ResetConnection();
}

void AdapterConnection::ClearLinkFlags()
{
    facade_.ResetConnection();
    facade_.SetIsIso14230Connection(false);
    facade_.SetIs29BitId(false);
    facade_.SetAddIso14230Header(false);
    facade_.SetIsCanConnection(false);
    facade_.SetIsIso15765Connection(false);
    facade_.SetSerialPortBaudrate("4800");
}

void AdapterConnection::ReturnToIdle()
{
    facade_.ResetConnection();
    facade_.SetSerialPortBaudrate("4800");
    facade_.SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::NoParity));
}

void AdapterConnection::SetPortSpeed(int baud)
{
    facade_.ChangePortSpeed(QString::number(baud));
}

std::optional<unsigned long> AdapterConnection::BatteryMillivolts()
{
    if (!facade_.GetUseOpenport2Adapter())
    {
        return std::nullopt;
    }
    return facade_.ReadVbatt();
}

void AdapterConnection::WaitForSource()
{
    facade_.waitForSource();
}

SerialPortActions& AdapterConnection::Facade()
{
    return facade_;
}

} // namespace fastecu::desktop::connection
