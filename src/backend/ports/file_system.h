#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/ports/result.h"

namespace fastecu
{

struct DirEntry
{
    std::string name;
    bool is_directory;
    std::int64_t modified_time_epoch_seconds;
};

// Directory provisioning. Distinct from IFileRepository (single-blob
// read/write by opaque handle) -- this is about creating and inspecting
// directories, a different capability.
class IFileSystem
{
  public:
    virtual ~IFileSystem() = default;
    virtual bool exists(std::string_view path) = 0;
    virtual Status create_directory(std::string_view path) = 0;
    virtual Status copy_file(std::string_view src, std::string_view dst, bool overwrite) = 0;
    virtual Status remove_file(std::string_view path) = 0;
    virtual Result<std::vector<DirEntry>> list_directory(std::string_view path) = 0;
};

} // namespace fastecu
