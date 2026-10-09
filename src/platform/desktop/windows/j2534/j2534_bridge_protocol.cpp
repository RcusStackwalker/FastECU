#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

namespace j2534_bridge
{

namespace
{

bool WriteAll(HANDLE pipe, const void *data, std::uint32_t size)
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

bool ReadAll(HANDLE pipe, void *data, std::uint32_t size)
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

bool WriteFrame(HANDLE pipe, Function function, const void *payload, std::uint32_t payload_size)
{
    FrameHeader header{function, payload_size};
    if (!WriteAll(pipe, &header, sizeof(header)))
    {
        return false;
    }
    if (payload_size == 0)
    {
        return true;
    }
    return WriteAll(pipe, payload, payload_size);
}

bool ReadFrameHeader(HANDLE pipe, FrameHeader& out_header)
{
    return ReadAll(pipe, &out_header, sizeof(out_header));
}

bool ReadFramePayload(HANDLE pipe, void *payload, std::uint32_t payload_size)
{
    if (payload_size == 0)
    {
        return true;
    }
    return ReadAll(pipe, payload, payload_size);
}

bool ReadFrame(HANDLE pipe, FrameHeader& out_header, void *payload, std::uint32_t payload_capacity)
{
    if (!ReadFrameHeader(pipe, out_header))
    {
        return false;
    }
    if (out_header.payload_size > payload_capacity)
    {
        return false;
    }
    return ReadFramePayload(pipe, payload, out_header.payload_size);
}

} // namespace j2534_bridge
