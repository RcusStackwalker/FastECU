// Windows bodies of SerialPortActionsDirect's per-OS hooks; see their
// declaration in serial_port_actions_direct.h. The BUILD file compiles
// exactly one of this file and serial_port_actions_direct_unix.cpp.
#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

#include <QSettings>

#include <algorithm>
#include <functional>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

namespace
{
constexpr auto kJ2534RegistryKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\PassThruSupport.04.04";

QMap<QString, QString> ReadJ2534RegistryView(QSettings::Format format)
{
    QSettings registry(kJ2534RegistryKey, format);
    QMap<QString, QString> drivers;
    for (const QString& group : registry.childGroups())
    {
        QString vendor = group;
        vendor.replace("\\", "/");
        drivers[vendor] = registry.value(group + "/FunctionLibrary").toString();
    }
    return drivers;
}
} // namespace

// Find first connected device
// TODO find all devices
QStringList SerialPortActionsDirect::CheckJ2534Devices(QMap<QString, QString> installed_drivers)
{
    bool j2534_device_found = false;
    QStringList j2534_devices;
    int driver_count = 0;
    for (const QString& vendor : installed_drivers.keys())
    {
        driver_count++;
        j2534_->Disable();
        // close_j2534_serial_port();
        QString j2534_dll_name = installed_drivers[vendor];
        emit logD("Testing for " + j2534_dll_name, true, true);
        j2534_->SetDllName(j2534_dll_name.toLocal8Bit().data());
        if (j2534_->Init())
        {
            emit logD(j2534_dll_name + " init successfull", true, true);
            // 0 means no error
            if (!j2534_->PassThruOpen(nullptr, &dev_id_))
            {
                emit logD("Successfully opened " + QString::number(dev_id_) + " / " + vendor + " / " + j2534_dll_name,
                          true, true);
                j2534_devices.append(vendor);
                j2534_device_found = true;
                j2534_->PassThruClose(dev_id_);
            }
            else
            {
                emit logE(QString::number(dev_id_) + " / " + vendor + " device not connected", true, true);
            }
        }
        else
        {
            emit logD(j2534_dll_name + " not found", true, true);
        }
        if (j2534_device_found)
        {
            break;
        }
    }
    emit logD("Tested installed drivers: " + QString::number(driver_count), true, true);

    return j2534_devices;
}

QMap<QString, QString> SerialPortActionsDirect::GetAllJ2534DriversNames()
{
    // Read the WOW64/32-bit registry view first so 32-bit-only J2534 vendors
    // are discoverable, then overlay the native 64-bit view so native
    // registrations win on vendor-name collisions.
    QMap<QString, QString> drivers_map = MergeJ2534DriverViews(ReadJ2534RegistryView(QSettings::Registry32Format),
                                                               ReadJ2534RegistryView(QSettings::Registry64Format));

    emit logD("Found installed drivers: ", true, false);
    for (const QString& dll_path : drivers_map)
    {
        emit logD(dll_path + ", ", false, false);
    }
    emit logD(" ", false, true);
    return drivers_map;
}

void SerialPortActionsDirect::ConnectJ2534Logs()
{
    // The Windows J2534 is not a QObject and emits no log signals.
}

void SerialPortActionsDirect::SettleAfterProgrammingVoltage()
{
}

void SerialPortActionsDirect::AppendJ2534Interfaces(QStringList& serial_ports)
{
    QStringList j2534_interfaces;
    installed_drivers_ = GetAllJ2534DriversNames();
    for (const QString installed_vendor : installed_drivers_.keys())
    {
        j2534_interfaces.append(installed_vendor);
    }
    std::sort(j2534_interfaces.begin(), j2534_interfaces.end(), std::less<QString>());
    serial_ports.append(j2534_interfaces);
}

SerialPortActionsDirect::ResolvedPort SerialPortActionsDirect::ResolvePort(const QString& entry) const
{
    const QString port = serial_port_prefix_win + entry;
    return {.port = port, .is_j2534 = !port.isEmpty()};
}

void SerialPortActionsDirect::SelectJ2534Dll()
{
    QString local_dll_name;
    QString installed_dll_name;

    QStringList dll_name = installed_drivers_.value(serial_port).split("\\");
    local_dll_name = dll_name.at(dll_name.count() - 1);
    installed_dll_name = installed_drivers_.value(serial_port);
    emit logD("Local DLL Name: " + local_dll_name, true, true);
    emit logD("Installed DLL Name: " + installed_dll_name, true, true);

    QMap<QString, QString> user_j2534_drivers;

    emit logD("Opening device: " + serial_port, true, true);
    QStringList j2534_driver;
    user_j2534_drivers[serial_port] = local_dll_name;
    j2534_driver = CheckJ2534Devices(user_j2534_drivers);
    user_j2534_drivers[serial_port] = installed_dll_name;
    if (j2534_driver.isEmpty())
    {
        j2534_driver = CheckJ2534Devices(user_j2534_drivers);
    }
    const QString resolved_dll_name = ResolveJ2534DllForConnection(serial_port, installed_dll_name, j2534_driver);
    if (!resolved_dll_name.isEmpty())
    {
        j2534_->SetDllName(resolved_dll_name.toLocal8Bit().data());
    }
    else
    {
        emit logD("Initializing interface failed!", true, true);
    }
}

bool SerialPortActionsDirect::OpenJ2534Transport()
{
    return true; // the Windows J2534 loads a DLL in init(); no port to open first
}

void SerialPortActionsDirect::CloseJ2534Transport()
{
}

void SerialPortActionsDirect::LogJ2534Opened()
{
}

void SerialPortActionsDirect::AdoptJ2534ChannelId()
{
}

bool SerialPortActionsDirect::J2534TxDone()
{
    return true;
}
