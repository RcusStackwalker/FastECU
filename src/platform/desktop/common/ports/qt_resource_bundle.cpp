#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include <QDirIterator>
#include <QFile>
#include <QString>

namespace
{
fastecu::Result<QString> prefix_for(std::string_view bundle_id)
{
    if (bundle_id == "config")
    {
        return QString(":/config/");
    }
    if (bundle_id == "kernels")
    {
        return QString(":/kernels/");
    }
    return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "unknown resource bundle id");
}
} // namespace

fastecu::Result<std::vector<std::string>> QtResourceBundle::list(std::string_view bundle_id)
{
    fastecu::Result<QString> prefix = prefix_for(bundle_id);
    if (!prefix.has_value())
    {
        return std::unexpected(prefix.error());
    }

    std::vector<std::string> names;
    QDirIterator it(*prefix, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        names.push_back(it.fileName().toStdString());
    }
    return names;
}

fastecu::Result<std::vector<std::uint8_t>> QtResourceBundle::read(std::string_view bundle_id,
                                                                  std::string_view name)
{
    fastecu::Result<QString> prefix = prefix_for(bundle_id);
    if (!prefix.has_value())
    {
        return std::unexpected(prefix.error());
    }

    QFile file(*prefix + QString::fromUtf8(name.data(), static_cast<int>(name.size())));
    if (!file.open(QIODevice::ReadOnly))
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "no such resource file");
    }
    QByteArray bytes = file.readAll();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}
