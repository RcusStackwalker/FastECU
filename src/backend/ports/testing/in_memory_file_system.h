#pragma once
#include "src/backend/ports/file_system.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fastecu
{

class InMemoryFileSystem : public IFileSystem
{
  public:
    bool exists(std::string_view path) override
    {
        return directories.count(std::string(path)) || files.count(std::string(path));
    }
    Status create_directory(std::string_view path) override
    {
        if (create_directory_error)
        {
            return std::unexpected(*create_directory_error);
        }
        directories.insert(std::string(path));
        return {};
    }
    Status copy_file(std::string_view src, std::string_view dst, bool overwrite) override
    {
        if (!files.count(std::string(src)))
        {
            return fastecu::fail(ErrorKind::Internal, "source missing");
        }
        std::string dst_str(dst);
        // Mirrors QFile::copy: no implicit mkpath. A destination whose parent
        // directory doesn't exist yet fails, it isn't silently created.
        auto slash = dst_str.find_last_of('/');
        if (slash != std::string::npos && !directories.count(dst_str.substr(0, slash + 1)))
        {
            return fastecu::fail(ErrorKind::Internal, "destination directory missing");
        }
        if (!overwrite && files.count(dst_str))
        {
            return fastecu::fail(ErrorKind::Internal, "destination exists");
        }
        files[dst_str] = files[std::string(src)];
        copy_calls.push_back({std::string(src), dst_str});
        return {};
    }
    Status remove_file(std::string_view path) override
    {
        directories.erase(std::string(path));
        files.erase(std::string(path));
        removed.push_back(std::string(path));
        return {};
    }
    Result<std::vector<DirEntry>> list_directory(std::string_view path) override
    {
        const std::string key(path);
        if (auto error = list_directory_errors.find(key); error != list_directory_errors.end())
        {
            return std::unexpected(error->second);
        }
        if (auto entries = directory_entries.find(key); entries != directory_entries.end())
        {
            return entries->second;
        }
        std::vector<DirEntry> entries;
        bool has_legacy_fixture = false;
        if (auto subdirectories = subdirectories_by_parent.find(key);
            subdirectories != subdirectories_by_parent.end())
        {
            has_legacy_fixture = true;
            for (const auto& [name, modified_time] : subdirectories->second)
            {
                entries.push_back(DirEntry{
                    .name = name,
                    .is_directory = true,
                    .modified_time_epoch_seconds = modified_time,
                });
            }
        }
        if (auto files = files_by_parent.find(key); files != files_by_parent.end())
        {
            has_legacy_fixture = true;
            for (const auto& [name, modified_time] : files->second)
            {
                entries.push_back(DirEntry{
                    .name = name,
                    .is_directory = false,
                    .modified_time_epoch_seconds = modified_time,
                });
            }
        }
        if (has_legacy_fixture)
        {
            return entries;
        }
        return fail(ErrorKind::InvalidConfig, "unknown directory: " + key);
    }

    std::set<std::string> directories;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, std::vector<DirEntry>> directory_entries;
    std::map<std::string, Error> list_directory_errors;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> subdirectories_by_parent;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> files_by_parent;
    std::vector<std::pair<std::string, std::string>> copy_calls;
    std::vector<std::string> removed;
    std::optional<Error> create_directory_error;
};

} // namespace fastecu
