#ifndef FASTECU_BYTES_H
#define FASTECU_BYTES_H

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <span>
#include <vector>

namespace bytes
{

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using ByteView = std::span<const Byte>;

inline void appendU16Be(Bytes& out, std::uint16_t value)
{
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>(value & 0xFF));
}

inline void appendU24Be(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>(value & 0xFF));
}

inline void appendU32Be(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>((value >> 24) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>(value & 0xFF));
}

inline void appendU16Le(Bytes& out, std::uint16_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
}

inline void appendU24Le(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 16) & 0xFF));
}

inline void appendU32Le(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 24) & 0xFF));
}

inline std::uint16_t readU16Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 2 > bytes.size())
        return 0;
    return static_cast<std::uint16_t>((std::uint16_t(bytes[offset]) << 8) | std::uint16_t(bytes[offset + 1]));
}

inline std::uint32_t readU24Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 3 > bytes.size())
        return 0;
    return (std::uint32_t(bytes[offset]) << 16) | (std::uint32_t(bytes[offset + 1]) << 8) | std::uint32_t(bytes[offset + 2]);
}

inline std::uint32_t readU32Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 4 > bytes.size())
        return 0;
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) | std::uint32_t(bytes[offset + 3]);
}

inline std::uint16_t readU16Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 2 > bytes.size())
        return 0;
    return static_cast<std::uint16_t>(std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8));
}

inline std::uint32_t readU24Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 3 > bytes.size())
        return 0;
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16);
}

inline std::uint32_t readU32Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 4 > bytes.size())
        return 0;
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

using MutableByteView = std::span<Byte>;

inline void writeU16Be(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset + 2 > out.size())
        return;
    out[offset] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 1] = static_cast<Byte>(value & 0xFF);
}

inline void writeU24Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 3 > out.size())
        return;
    out[offset] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>(value & 0xFF);
}

inline void writeU32Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 4 > out.size())
        return;
    out[offset] = static_cast<Byte>((value >> 24) & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 3] = static_cast<Byte>(value & 0xFF);
}

inline void writeU16Le(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset + 2 > out.size())
        return;
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
}

inline void writeU24Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 3 > out.size())
        return;
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
}

inline void writeU32Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 4 > out.size())
        return;
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<Byte>((value >> 24) & 0xFF);
}

inline Byte sum8(ByteView bytes, std::size_t from, std::size_t len)
{
    if (from >= bytes.size())
        return 0;
    std::uint32_t sum = 0;
    const std::size_t end = std::min(bytes.size(), from + std::min(len, bytes.size() - from));
    for (std::size_t i = from; i < end; ++i)
        sum += bytes[i];
    return static_cast<Byte>(sum & 0xFF);
}

inline Byte sum8(ByteView bytes)
{
    return sum8(bytes, 0, bytes.size());
}

} // namespace bytes

#endif // FASTECU_BYTES_H
