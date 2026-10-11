#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QSerialPort>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::desktop::serial
{

void ResetSerialToIdle(SerialPortActions& serial)
{
    serial.ResetConnection();
    serial.SetIsIso14230Connection(false);
    serial.SetIs29BitId(false);
    serial.SetAddIso14230Header(false);
    serial.SetIsCanConnection(false);
    serial.SetIsIso15765Connection(false);
    serial.SetSerialPortParity(QSerialPort::NoParity);
    serial.SetSerialPortBaudrate("4800");
}

} // namespace fastecu::desktop::serial
