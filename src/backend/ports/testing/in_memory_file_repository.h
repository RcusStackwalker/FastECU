#pragma once
#include "src/backend/ports/file_repository.h"

#include <map>
#include <string>

namespace fastecu
{

class InMemoryFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view h) override
    {
        auto it = files.find(std::string(h));
        if (it == files.end())
        {
            return fail(ErrorKind::InvalidConfig, "no such handle");
        }
        return it->second;
    }
    Status write(std::string_view h, std::span<const std::uint8_t> d) override
    {
        files[std::string(h)].assign(d.begin(), d.end());
        return {};
    }
    std::map<std::string, std::vector<std::uint8_t>> files;
};

} // namespace fastecu
