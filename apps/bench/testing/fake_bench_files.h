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
    std::map<std::string, int> load_calls;
    bool fail_on_repeated_load = false;

    Result<bytes::Bytes> load(std::string_view path) override
    {
        const std::string key(path);
        if (const int call = ++load_calls[key]; fail_on_repeated_load && call > 1)
        {
            return fail(ErrorKind::InvalidConfig, "file was loaded more than once");
        }
        const auto found = contents.find(key);
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
