#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include <QFile>
#include <QString>

fastecu::Result<std::vector<std::uint8_t>> QtFileRepository::read(std::string_view handle)
{
    QFile f(QString::fromUtf8(handle.data(), static_cast<int>(handle.size())));
    if (!f.open(QIODevice::ReadOnly))
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "cannot open file");
    QByteArray a = f.readAll();
    return std::vector<std::uint8_t>(a.begin(), a.end());
}

fastecu::Status QtFileRepository::write(std::string_view handle, std::span<const std::uint8_t> d)
{
    QFile f(QString::fromUtf8(handle.data(), static_cast<int>(handle.size())));
    if (!f.open(QIODevice::WriteOnly))
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "cannot open file");
    qint64 n = f.write(reinterpret_cast<const char *>(d.data()), static_cast<qint64>(d.size()));
    if (n != static_cast<qint64>(d.size()))
        return fastecu::fail(fastecu::ErrorKind::Internal, "short write");
    return {};
}
