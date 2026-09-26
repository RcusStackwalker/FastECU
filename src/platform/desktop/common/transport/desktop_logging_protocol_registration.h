#pragma once

class SerialPortActions;
namespace fastecu
{
class IClock;
}
namespace fastecu::desktop::logging
{
class LoggingEngine;

// Registers factories without performing I/O. serial and clock must outlive
// engine: its factories and active protocols retain references to them.
void register_desktop_logging_protocols(LoggingEngine& engine, SerialPortActions& serial, fastecu::IClock& clock);
} // namespace fastecu::desktop::logging
