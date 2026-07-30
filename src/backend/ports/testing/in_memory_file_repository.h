#pragma once
#include "src/backend/ports/file_repository.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastecu
{

class InMemoryFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view h) override
    {
        read_handles.push_back(std::string(h));
        if (next_read_result)
        {
            auto result = std::move(*next_read_result);
            next_read_result.reset();
            return result;
        }
        const std::string key(h);
        if (auto error = read_errors.find(key); error != read_errors.end())
        {
            return std::unexpected(error->second);
        }
        auto it = files.find(key);
        if (it == files.end())
        {
            return fail(ErrorKind::InvalidConfig, "no such handle");
        }
        return it->second;
    }
    int read_count(std::string_view handle) const
    {
        return static_cast<int>(std::count(
            read_handles.begin(), read_handles.end(), std::string(handle)));
    }
    Status write(std::string_view h, std::span<const std::uint8_t> d) override
    {
        std::vector<std::uint8_t> data(d.begin(), d.end());
        write_calls.push_back({std::string(h), data});
        files[std::string(h)] = std::move(data);
        return {};
    }
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, Error> read_errors;
    std::optional<Result<std::vector<std::uint8_t>>> next_read_result;
    std::vector<std::string> read_handles;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> write_calls;
};

} // namespace fastecu
