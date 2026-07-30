#include "src/platform/desktop/common/ports/qt_file_system.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace
{
QString to_qstring(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}
} // namespace

bool QtFileSystem::exists(std::string_view path)
{
    return QFileInfo::exists(to_qstring(path));
}

fastecu::Status QtFileSystem::create_directory(std::string_view path)
{
    if (!QDir().mkpath(to_qstring(path)))
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "mkpath failed");
    }
    return {};
}

fastecu::Status QtFileSystem::copy_file(std::string_view src, std::string_view dst, bool overwrite)
{
    const QString qdst = to_qstring(dst);
    if (overwrite && QFileInfo::exists(qdst))
    {
        QFile::remove(qdst);
    }
    if (!overwrite && QFileInfo::exists(qdst))
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "destination exists");
    }
    if (!QFile::copy(to_qstring(src), qdst))
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "copy failed");
    }
    return {};
}

fastecu::Status QtFileSystem::remove_file(std::string_view path)
{
    if (!QFile::remove(to_qstring(path)))
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "remove failed");
    }
    return {};
}

fastecu::Result<std::vector<fastecu::DirEntry>> QtFileSystem::list_directory(std::string_view path)
{
    QDir dir(to_qstring(path));
    if (!dir.exists())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "directory does not exist");
    }
    std::vector<fastecu::DirEntry> entries;
    const QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : list)
    {
        entries.push_back(fastecu::DirEntry{
            info.fileName().toStdString(),
            info.isDir(),
            info.lastModified().toSecsSinceEpoch(),
            info.isSymLink(),
        });
    }
    return entries;
}
