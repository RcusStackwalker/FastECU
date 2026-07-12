#include "j2534_bridge_protocol.h"

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
            return false;
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
            return false;
        read += chunk;
    }
    return true;
}

} // namespace

bool writeFrame(HANDLE pipe, Function function, const void *payload, std::uint32_t payloadSize)
{
    FrameHeader header{function, payloadSize};
    if (!writeAll(pipe, &header, sizeof(header)))
        return false;
    if (payloadSize == 0)
        return true;
    return writeAll(pipe, payload, payloadSize);
}

bool readFrame(HANDLE pipe, FrameHeader& outHeader, void *payload, std::uint32_t payloadCapacity)
{
    if (!readAll(pipe, &outHeader, sizeof(outHeader)))
        return false;
    if (outHeader.payloadSize == 0)
        return true;
    if (outHeader.payloadSize > payloadCapacity)
        return false;
    return readAll(pipe, payload, outHeader.payloadSize);
}

} // namespace j2534_bridge
