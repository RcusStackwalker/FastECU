#pragma once
#include "src/backend/ports/file_system.h"

// QDir/QFile-backed IFileSystem implementation.
class QtFileSystem : public fastecu::IFileSystem
{
  public:
    bool Exists(std::string_view path) override;
    fastecu::Status MakeDirectory(std::string_view path) override;
    fastecu::Status CopyFileTo(std::string_view src, std::string_view dst, bool overwrite) override;
    fastecu::Status RemoveFile(std::string_view path) override;
    fastecu::Result<std::vector<fastecu::DirEntry>> ListDirectory(std::string_view path) override;
};
