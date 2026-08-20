#pragma once
#include <map>
#include <string>

#include "apps/bench/bench_session_interface.h"

namespace fastecu::bench::testing
{

// In-memory stand-in for IBenchFiles: pre-seed `contents` for load(), and
// inspect `saved` afterward for what a command wrote.
class FakeBenchFiles : public IBenchFiles
{
  public:
    std::map<std::string, bytes::Bytes> contents;
    std::map<std::string, bytes::Bytes> saved;

    Result<bytes::Bytes> load(std::string_view path) override
    {
        const auto found = contents.find(std::string(path));
        if (found == contents.end())
        {
            return fail(ErrorKind::InvalidConfig, "no such file");
        }
        return found->second;
    }

    Status save(std::string_view path, bytes::ByteView data) override
    {
        saved[std::string(path)] = bytes::Bytes(data.begin(), data.end());
        return {};
    }
};

} // namespace fastecu::bench::testing
