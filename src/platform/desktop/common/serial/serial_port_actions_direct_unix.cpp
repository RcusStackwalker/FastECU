// Unix bodies of SerialPortActionsDirect's per-OS hooks; see their
// declaration in serial_port_actions_direct.h. The BUILD file compiles
// exactly one of this file and serial_port_actions_direct_windows.cpp.
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

void SerialPortActionsDirect::connect_j2534_logs()
{
    QObject::connect(j2534, &J2534::LOG_E, this, &SerialPortActionsDirect::LOG_E);
    QObject::connect(j2534, &J2534::LOG_W, this, &SerialPortActionsDirect::LOG_W);
    QObject::connect(j2534, &J2534::LOG_I, this, &SerialPortActionsDirect::LOG_I);
    QObject::connect(j2534, &J2534::LOG_D, this, &SerialPortActionsDirect::LOG_D);
}

void SerialPortActionsDirect::settle_after_programming_voltage()
{
    delay(1);
}

void SerialPortActionsDirect::append_j2534_interfaces(QStringList& /*serial_ports*/)
{
    // A Unix adapter enumerates as a serial port; there is no driver registry.
}

SerialPortActionsDirect::ResolvedPort SerialPortActionsDirect::resolve_port(const QString& entry) const
{
    const QString prefixed = serial_port_prefix_linux + entry;
    return {.port = prefixed.split(" - ").at(0), .is_j2534 = isJ2534CapableEntry(prefixed)};
}

void SerialPortActionsDirect::select_j2534_dll()
{
    // The Unix J2534 drives the adapter's serial port; there is no DLL to pick.
}

bool SerialPortActionsDirect::open_j2534_transport()
{
    return j2534->open_serial_port(serial_port) == serial_port;
}

void SerialPortActionsDirect::close_j2534_transport()
{
    j2534->close_serial_port();
}

void SerialPortActionsDirect::log_j2534_opened()
{
    emit LOG_D("INIT: J2534 opened with devID: " + QString::number(devID), true, true);
}

void SerialPortActionsDirect::adopt_j2534_channel_id()
{
    chanID = protocol;
}

bool SerialPortActionsDirect::j2534_tx_done()
{
    return j2534->get_is_tx_done();
}
