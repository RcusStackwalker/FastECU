#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

inline QString resolveJ2534DllForConnection(const QString& selectedVendor,
                                            const QString& installedDllName,
                                            const QStringList& detectedDrivers)
{
    return detectedDrivers.contains(selectedVendor) ? installedDllName : QString();
}

// Merges vendor -> DLL-path maps from multiple registry views (e.g. the
// native view and the Wow6432Node view a 32-bit-only J2534 vendor may have
// registered under instead). Earlier entries in registryViews win on a
// vendor-name collision -- pure function, no registry access, fully
// unit-testable independent of QSettings/Windows.
inline QMap<QString, QString> mergeJ2534DriverViews(const QList<QMap<QString, QString>>& registryViews)
{
    QMap<QString, QString> merged;
    for (const auto& view : registryViews)
    {
        for (auto it = view.constBegin(); it != view.constEnd(); ++it)
        {
            if (!merged.contains(it.key()))
                merged[it.key()] = it.value();
        }
    }
    return merged;
}
