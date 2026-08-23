#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtGlobal>

#include <utility>

// True when a check_serial_ports() entry names a device
// SerialPortActionsDirect::open_serial_port() will drive through J2534 rather
// than degrade to a plain serial port.
//
// Lives next to the entry format so the two cannot drift: on Unix an entry is
// "<portName> - <description>", and only the description tells an adapter from
// a Bluetooth or debug-console port. Windows entries come from
// getAllJ2534DriversNames() and carry no description; open_serial_port()
// already treats every one of them as J2534-capable, so this must stay true
// there or it regresses Windows.
inline bool isJ2534CapableEntry(QStringView entry)
{
#if defined Q_OS_UNIX
    const qsizetype separator = entry.indexOf(QStringView(u" - "));
    return separator >= 0 && entry.sliced(separator + 3).contains(QStringView(u"OpenPort 2.0"), Qt::CaseInsensitive);
#else
    return !entry.isEmpty();
#endif
}

inline QString resolveJ2534DllForConnection(const QString& selectedVendor, const QString& installedDllName,
                                            const QStringList& detectedDrivers)
{
    return detectedDrivers.contains(selectedVendor) ? installedDllName : QString();
}

// Merges vendor -> DLL-path maps from multiple registry views. Later entries
// overwrite earlier values on vendor-name collision, matching the production
// Registry32Format base + Registry64Format overlay order.
template <typename... RegistryViews>
inline QMap<QString, QString> mergeJ2534DriverViews(QMap<QString, QString> firstView, RegistryViews... registryViews)
{
    QMap<QString, QString> merged = std::move(firstView);
    const auto overlay = [&merged](QMap<QString, QString>& view)
    {
        for (auto&& [vendor, dllPath] : view.asKeyValueRange())
        {
            merged[vendor] = std::move(dllPath);
        }
    };

    (overlay(registryViews), ...);
    return merged;
}
