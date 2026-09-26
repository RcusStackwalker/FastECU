#pragma once

class SerialPortActions;

namespace fastecu::desktop::serial
{

// Resets the connection and restores the idle line state MainWindow uses
// between ECU operations: every protocol mode off, 11-bit ids, no parity,
// 4800 baud.
void reset_serial_to_idle(SerialPortActions& serial);

} // namespace fastecu::desktop::serial
