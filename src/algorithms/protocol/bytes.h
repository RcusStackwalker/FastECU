#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <span>
#include <string>
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
    if (offset > bytes.size() || 2 > bytes.size() - offset)
    {
        return 0;
    }
    return static_cast<std::uint16_t>((std::uint16_t(bytes[offset]) << 8) | std::uint16_t(bytes[offset + 1]));
}

inline std::uint32_t readU24Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 3 > bytes.size() - offset)
    {
        return 0;
    }
    return (std::uint32_t(bytes[offset]) << 16) | (std::uint32_t(bytes[offset + 1]) << 8) |
           std::uint32_t(bytes[offset + 2]);
}

inline std::uint32_t readU32Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 4 > bytes.size() - offset)
    {
        return 0;
    }
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | std::uint32_t(bytes[offset + 3]);
}

inline std::uint16_t readU16Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 2 > bytes.size() - offset)
    {
        return 0;
    }
    return static_cast<std::uint16_t>(std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8));
}

inline std::uint32_t readU24Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 3 > bytes.size() - offset)
    {
        return 0;
    }
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16);
}

inline std::uint32_t readU32Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 4 > bytes.size() - offset)
    {
        return 0;
    }
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

// Reads `width` bytes (1-4) at `offset`, most-significant byte first.
// Dispatches to the fixed-width readers above, and inherits their
// return-0-when-out-of-range convention; a `width` outside 1-4 also yields 0.
//
// Exists because callers that pick their width at runtime from a definition's
// storage type (src/backend/calibration/calibration_service.cpp) would
// otherwise hand-roll endian normalization and MSB-first assembly themselves.
inline std::uint32_t readUBe(ByteView bytes, std::size_t offset, std::uint32_t width)
{
    switch (width)
    {
    case 1:
        return offset < bytes.size() ? std::uint32_t(bytes[offset]) : 0u;
    case 2:
        return readU16Be(bytes, offset);
    case 3:
        return readU24Be(bytes, offset);
    case 4:
        return readU32Be(bytes, offset);
    default:
        return 0u;
    }
}

// Least-significant byte first. See readUBe.
inline std::uint32_t readULe(ByteView bytes, std::size_t offset, std::uint32_t width)
{
    switch (width)
    {
    case 1:
        return offset < bytes.size() ? std::uint32_t(bytes[offset]) : 0u;
    case 2:
        return readU16Le(bytes, offset);
    case 3:
        return readU24Le(bytes, offset);
    case 4:
        return readU32Le(bytes, offset);
    default:
        return 0u;
    }
}

using MutableByteView = std::span<Byte>;

inline void overwriteAt(MutableByteView out, std::size_t offset, ByteView payload)
{
    if (offset >= out.size())
    {
        return;
    }
    const auto count = std::min(payload.size(), out.size() - offset);
    std::copy_n(payload.begin(), count, out.begin() + static_cast<std::ptrdiff_t>(offset));
}

inline void writeU16Be(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset > out.size() || 2 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 1] = static_cast<Byte>(value & 0xFF);
}

inline void writeU24Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 3 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>(value & 0xFF);
}

inline void writeU32Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 4 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>((value >> 24) & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 3] = static_cast<Byte>(value & 0xFF);
}

inline void writeU16Le(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset > out.size() || 2 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
}

inline void writeU24Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 3 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
}

inline void writeU32Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 4 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<Byte>((value >> 24) & 0xFF);
}

// Sums `len` bytes starting at `from`, clamping `len` to what is available.
// Named distinctly from sum8(ByteView) so that sum8 remains a single
// function and can therefore be passed as a callable (see
// composeBeWithChecksum in bytes_compose.h).
inline Byte sum8Range(ByteView bytes, std::size_t from, std::size_t len)
{
    if (from >= bytes.size())
    {
        return 0;
    }
    const auto slice = bytes.subspan(from, std::min(len, bytes.size() - from));
    const auto sum = std::accumulate(slice.begin(), slice.end(), 0u);
    return static_cast<Byte>(sum & 0xFF);
}

inline Byte sum8(ByteView bytes)
{
    return sum8Range(bytes, 0, bytes.size());
}

// Renders each byte as two lowercase hex digits followed by a space
// (e.g. "80 01 02 ff "). Debug-log formatting, not a protocol detail.
inline std::string toHex(ByteView bytes)
{
    std::string msg;
    msg.reserve(bytes.size() * 3);
    char hex[4] = {};
    for (const Byte byte : bytes)
    {
        std::snprintf(hex, sizeof(hex), "%02x ", byte);
        msg.append(hex);
    }
    return msg;
}

} // namespace bytes
