#include "src/backend/ports/file_system.h"

#include <cstdint>
#include <map>
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
        directories.insert(std::string(path));
        return {};
    }
    Status copy_file(std::string_view src, std::string_view dst, bool overwrite) override
    {
        if (!files.count(std::string(src)))
            return fastecu::fail(ErrorKind::Internal, "source missing");
        std::string dst_str(dst);
        // Mirrors QFile::copy: no implicit mkpath. A destination whose parent
        // directory doesn't exist yet fails, it isn't silently created.
        auto slash = dst_str.find_last_of('/');
        if (slash != std::string::npos && !directories.count(dst_str.substr(0, slash + 1)))
            return fastecu::fail(ErrorKind::Internal, "destination directory missing");
        if (!overwrite && files.count(dst_str))
            return fastecu::fail(ErrorKind::Internal, "destination exists");
        files[dst_str] = files[std::string(src)];
        copy_calls.push_back({std::string(src), dst_str});
        return {};
    }
    Status remove_file(std::string_view path) override
    {
        files.erase(std::string(path));
        removed.push_back(std::string(path));
        return {};
    }
    Result<std::vector<DirEntry>> list_directory(std::string_view path) override
    {
        std::vector<DirEntry> out;
        for (auto& [name, mtime] : subdirectories_by_parent[std::string(path)])
            out.push_back(DirEntry{name, true, mtime});
        for (auto& [name, mtime] : files_by_parent[std::string(path)])
            out.push_back(DirEntry{name, false, mtime});
        return out;
    }

    std::set<std::string> directories;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> subdirectories_by_parent;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> files_by_parent;
    std::vector<std::pair<std::string, std::string>> copy_calls;
    std::vector<std::string> removed;
};

} // namespace fastecu
