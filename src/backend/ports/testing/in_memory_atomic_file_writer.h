#pragma once

#include "src/backend/ports/atomic_file_writer.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fastecu
{

struct ReplaceCall
{
    std::string handle;
    std::vector<std::uint8_t> data;

    bool operator==(const ReplaceCall&) const = default;
};

class InMemoryAtomicFileWriter : public IAtomicFileWriter
{
  public:
    Status replace(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        ReplaceCall call{
            .handle = std::string(handle),
            .data = std::vector<std::uint8_t>(data.begin(), data.end()),
        };
        replace_calls.push_back(call);
        if (replace_error)
        {
            return std::unexpected(*replace_error);
        }
        files[call.handle] = std::move(call.data);
        return {};
    }

    void reset()
    {
        replace_calls.clear();
        files.clear();
        replace_error.reset();
    }

    std::vector<ReplaceCall> replace_calls;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::optional<Error> replace_error;
};

} // namespace fastecu
