#include "src/platform/desktop/common/ports/qt_file_system.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace
{
QString ToQstring(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}
} // namespace

bool QtFileSystem::Exists(std::string_view path)
{
    return QFileInfo::exists(ToQstring(path));
}

fastecu::Status QtFileSystem::CreateDirectory(std::string_view path)
{
    if (!QDir().mkpath(ToQstring(path)))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "mkpath failed");
    }
    return {};
}

fastecu::Status QtFileSystem::CopyFile(std::string_view src, std::string_view dst, bool overwrite)
{
    const QString qdst = ToQstring(dst);
    if (overwrite && QFileInfo::exists(qdst))
    {
        QFile::remove(qdst);
    }
    if (!overwrite && QFileInfo::exists(qdst))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "destination exists");
    }
    if (!QFile::copy(ToQstring(src), qdst))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "copy failed");
    }
    return {};
}

fastecu::Status QtFileSystem::RemoveFile(std::string_view path)
{
    if (!QFile::remove(ToQstring(path)))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "remove failed");
    }
    return {};
}

fastecu::Result<std::vector<fastecu::DirEntry>> QtFileSystem::ListDirectory(std::string_view path)
{
    QDir dir(ToQstring(path));
    if (!dir.exists())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "directory does not exist");
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
