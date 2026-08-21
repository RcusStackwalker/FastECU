#pragma once
#include "apps/bench/bench_session_interface.h"

namespace fastecu::bench
{

// std::ifstream/ofstream-backed IBenchFiles: plain iostream, no Qt, so
// download/dump/upload-routine's --from can run without QFile.
class BenchFiles final : public IBenchFiles
{
  public:
    Result<bytes::Bytes> load(std::string_view path) override;
    Status save(std::string_view path, bytes::ByteView data) override;
};

} // namespace fastecu::bench
