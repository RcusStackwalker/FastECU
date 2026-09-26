#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QSerialPort>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::desktop::serial
{

void reset_serial_to_idle(SerialPortActions& serial)
{
    serial.reset_connection();
    serial.set_is_iso14230_connection(false);
    serial.set_is_29_bit_id(false);
    serial.set_add_iso14230_header(false);
    serial.set_is_can_connection(false);
    serial.set_is_iso15765_connection(false);
    serial.set_serial_port_parity(QSerialPort::NoParity);
    serial.set_serial_port_baudrate("4800");
}

} // namespace fastecu::desktop::serial
