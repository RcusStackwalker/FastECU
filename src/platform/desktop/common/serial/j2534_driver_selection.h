#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

#include <utility>

inline QString resolveJ2534DllForConnection(const QString& selectedVendor,
                                            const QString& installedDllName,
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
            merged[vendor] = std::move(dllPath);
    };

    (overlay(registryViews), ...);
    return merged;
}
