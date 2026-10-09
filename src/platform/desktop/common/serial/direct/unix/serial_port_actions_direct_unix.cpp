// Unix bodies of SerialPortActionsDirect's per-OS hooks; see their
// declaration in serial_port_actions_direct.h. The BUILD file compiles
// exactly one of this file and serial_port_actions_direct_windows.cpp.
#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

void SerialPortActionsDirect::ConnectJ2534Logs()
{
    QObject::connect(j2534_, &J2534::LOG_E, this, &SerialPortActionsDirect::LOG_E);
    QObject::connect(j2534_, &J2534::LOG_W, this, &SerialPortActionsDirect::LOG_W);
    QObject::connect(j2534_, &J2534::LOG_I, this, &SerialPortActionsDirect::LOG_I);
    QObject::connect(j2534_, &J2534::LOG_D, this, &SerialPortActionsDirect::LOG_D);
}

void SerialPortActionsDirect::SettleAfterProgrammingVoltage()
{
    delay(1);
}

void SerialPortActionsDirect::AppendJ2534Interfaces(QStringList& /*serial_ports*/)
{
    // A Unix adapter enumerates as a serial port; there is no driver registry.
}

SerialPortActionsDirect::ResolvedPort SerialPortActionsDirect::ResolvePort(const QString& entry) const
{
    const QString prefixed = serial_port_prefix_linux + entry;
    return {.port = prefixed.split(" - ").at(0), .is_j2534 = IsJ2534CapableEntry(prefixed)};
}

void SerialPortActionsDirect::SelectJ2534Dll()
{
    // The Unix J2534 drives the adapter's serial port; there is no DLL to pick.
}

bool SerialPortActionsDirect::OpenJ2534Transport()
{
    return j2534_->OpenSerialPort(serial_port) == serial_port;
}

void SerialPortActionsDirect::CloseJ2534Transport()
{
    j2534_->CloseSerialPort();
}

void SerialPortActionsDirect::LogJ2534Opened()
{
    emit LOG_D("INIT: J2534 opened with devID: " + QString::number(dev_id_), true, true);
}

void SerialPortActionsDirect::AdoptJ2534ChannelId()
{
    chan_id_ = protocol_;
}

bool SerialPortActionsDirect::J2534TxDone()
{
    return j2534_->GetIsTxDone();
}
