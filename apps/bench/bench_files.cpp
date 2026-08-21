#include "apps/bench/bench_files.h"

#include <format>
#include <fstream>
#include <iterator>

namespace fastecu::bench
{

Result<bytes::Bytes> BenchFiles::load(std::string_view path)
{
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file.is_open())
    {
        return fail(ErrorKind::InvalidConfig, std::format("cannot open {} for reading", path));
    }
    bytes::Bytes data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad())
    {
        return fail(ErrorKind::InvalidConfig, std::format("read error on {}", path));
    }
    return data;
}

Status BenchFiles::save(std::string_view path, bytes::ByteView data)
{
    std::ofstream file(std::string(path), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return fail(ErrorKind::Internal, std::format("cannot open {} for writing", path));
    }
    // ostream::write needs char*; matches the reinterpret_cast<const char*> pattern already used at every other
    // bytes->file boundary in this codebase (e.g. qt_atomic_file_writer.cpp).
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file)
    {
        return fail(ErrorKind::Internal, std::format("write error on {}", path));
    }
    return {};
}

} // namespace fastecu::bench
