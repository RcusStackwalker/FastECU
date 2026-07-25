#pragma once
#include "src/backend/ports/file_system.h"

// QDir/QFile-backed IFileSystem implementation.
class QtFileSystem : public fastecu::IFileSystem
{
  public:
    bool exists(std::string_view path) override;
    fastecu::Status create_directory(std::string_view path) override;
    fastecu::Status copy_file(std::string_view src, std::string_view dst, bool overwrite) override;
    fastecu::Status remove_file(std::string_view path) override;
    fastecu::Result<std::vector<fastecu::DirEntry>> list_directory(std::string_view path) override;
};
