// Windows bodies of SerialPortActionsDirect's per-OS hooks; see their
// declaration in serial_port_actions_direct.h. The BUILD file compiles
// exactly one of this file and serial_port_actions_direct_unix.cpp.
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

#include <QSettings>

#include <algorithm>
#include <functional>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

namespace
{
constexpr auto kJ2534RegistryKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\PassThruSupport.04.04";

QMap<QString, QString> readJ2534RegistryView(QSettings::Format format)
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
QStringList SerialPortActionsDirect::check_j2534_devices(QMap<QString, QString> installed_drivers)
{
    bool j2534DeviceFound = false;
    QStringList j2534_devices;
    int driver_count = 0;
    for (const QString& vendor : installed_drivers.keys())
    {
        driver_count++;
        j2534->disable();
        // close_j2534_serial_port();
        QString j2534DllName = installed_drivers[vendor];
        emit LOG_D("Testing for " + j2534DllName, true, true);
        j2534->setDllName(j2534DllName.toLocal8Bit().data());
        if (j2534->init())
        {
            emit LOG_D(j2534DllName + " init successfull", true, true);
            // 0 means no error
            if (!j2534->PassThruOpen(nullptr, &devID))
            {
                emit LOG_D("Successfully opened " + QString::number(devID) + " / " + vendor + " / " + j2534DllName,
                           true, true);
                j2534_devices.append(vendor);
                j2534DeviceFound = true;
                j2534->PassThruClose(devID);
            }
            else
                emit LOG_E(QString::number(devID) + " / " + vendor + " device not connected", true, true);
        }
        else
            emit LOG_D(j2534DllName + " not found", true, true);
        if (j2534DeviceFound)
            break;
    }
    emit LOG_D("Tested installed drivers: " + QString::number(driver_count), true, true);

    return j2534_devices;
}

QMap<QString, QString> SerialPortActionsDirect::getAllJ2534DriversNames()
{
    // Read the WOW64/32-bit registry view first so 32-bit-only J2534 vendors
    // are discoverable, then overlay the native 64-bit view so native
    // registrations win on vendor-name collisions.
    QMap<QString, QString> drivers_map = mergeJ2534DriverViews(readJ2534RegistryView(QSettings::Registry32Format),
                                                               readJ2534RegistryView(QSettings::Registry64Format));

    emit LOG_D("Found installed drivers: ", true, false);
    for (const QString& dllPath : drivers_map)
        emit LOG_D(dllPath + ", ", false, false);
    emit LOG_D(" ", false, true);
    return drivers_map;
}

void SerialPortActionsDirect::connect_j2534_logs()
{
    // The Windows J2534 is not a QObject and emits no log signals.
}

void SerialPortActionsDirect::settle_after_programming_voltage()
{
}

void SerialPortActionsDirect::append_j2534_interfaces(QStringList& serial_ports)
{
    QStringList j2534_interfaces;
    installed_drivers = getAllJ2534DriversNames();
    for (const QString installed_vendor : installed_drivers.keys())
    {
        j2534_interfaces.append(installed_vendor);
    }
    std::sort(j2534_interfaces.begin(), j2534_interfaces.end(), std::less<QString>());
    serial_ports.append(j2534_interfaces);
}

SerialPortActionsDirect::ResolvedPort SerialPortActionsDirect::resolve_port(const QString& entry) const
{
    const QString port = serial_port_prefix_win + entry;
    return {.port = port, .is_j2534 = !port.isEmpty()};
}

void SerialPortActionsDirect::select_j2534_dll()
{
    QString localDllName;
    QString installedDllName;

    QStringList dllName = installed_drivers.value(serial_port).split("\\");
    localDllName = dllName.at(dllName.count() - 1);
    installedDllName = installed_drivers.value(serial_port);
    emit LOG_D("Local DLL Name: " + localDllName, true, true);
    emit LOG_D("Installed DLL Name: " + installedDllName, true, true);

    QMap<QString, QString> user_j2534_drivers;

    emit LOG_D("Opening device: " + serial_port, true, true);
    QStringList j2534_driver;
    user_j2534_drivers[serial_port] = localDllName;
    j2534_driver = check_j2534_devices(user_j2534_drivers);
    user_j2534_drivers[serial_port] = installedDllName;
    if (j2534_driver.isEmpty())
        j2534_driver = check_j2534_devices(user_j2534_drivers);
    const QString resolvedDllName = resolveJ2534DllForConnection(serial_port, installedDllName, j2534_driver);
    if (!resolvedDllName.isEmpty())
        j2534->setDllName(resolvedDllName.toLocal8Bit().data());
    else
        emit LOG_D("Initializing interface failed!", true, true);
}

bool SerialPortActionsDirect::open_j2534_transport()
{
    return true; // the Windows J2534 loads a DLL in init(); no port to open first
}

void SerialPortActionsDirect::close_j2534_transport()
{
}

void SerialPortActionsDirect::log_j2534_opened()
{
}

void SerialPortActionsDirect::adopt_j2534_channel_id()
{
}

bool SerialPortActionsDirect::j2534_tx_done()
{
    return true;
}
