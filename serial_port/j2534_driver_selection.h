#pragma once

#include <QString>
#include <QStringList>

inline QString resolveJ2534DllForConnection(const QString& selectedVendor,
                                            const QString& installedDllName,
                                            const QStringList& detectedDrivers)
{
    return detectedDrivers.contains(selectedVendor) ? installedDllName : QString();
}
