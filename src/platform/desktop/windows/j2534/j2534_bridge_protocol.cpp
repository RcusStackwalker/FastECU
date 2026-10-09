#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

namespace j2534_bridge
{

namespace
{

bool writeAll(HANDLE pipe, const void *data, std::uint32_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::uint32_t written = 0;
    while (written < size)
    {
        DWORD chunk = 0;
        if (!WriteFile(pipe, bytes + written, size - written, &chunk, nullptr) || chunk == 0)
        {
            return false;
        }
        written += chunk;
    }
    return true;
}

bool readAll(HANDLE pipe, void *data, std::uint32_t size)
{
    auto *bytes = static_cast<unsigned char *>(data);
    std::uint32_t read = 0;
    while (read < size)
    {
        DWORD chunk = 0;
        if (!ReadFile(pipe, bytes + read, size - read, &chunk, nullptr) || chunk == 0)
        {
            return false;
        }
        read += chunk;
    }
    return true;
}

} // namespace

bool writeFrame(HANDLE pipe, Function function, const void *payload, std::uint32_t payload_size)
{
    FrameHeader header{function, payload_size};
    if (!writeAll(pipe, &header, sizeof(header)))
    {
        return false;
    }
    if (payload_size == 0)
    {
        return true;
    }
    return writeAll(pipe, payload, payload_size);
}

bool readFrameHeader(HANDLE pipe, FrameHeader& out_header)
{
    return readAll(pipe, &out_header, sizeof(out_header));
}

bool readFramePayload(HANDLE pipe, void *payload, std::uint32_t payload_size)
{
    if (payload_size == 0)
    {
        return true;
    }
    return readAll(pipe, payload, payload_size);
}

bool readFrame(HANDLE pipe, FrameHeader& out_header, void *payload, std::uint32_t payload_capacity)
{
    if (!readFrameHeader(pipe, out_header))
    {
        return false;
    }
    if (out_header.payload_size > payload_capacity)
    {
        return false;
    }
    return readFramePayload(pipe, payload, out_header.payload_size);
}

} // namespace j2534_bridge
