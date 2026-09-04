#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <format>
#include <numeric>
#include <span>
#include <string>
#include <vector>
#include <array>

namespace bytes
{

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using ByteView = std::span<const Byte>;

inline void appendU16Be(Bytes& out, std::uint16_t value)
{
    const auto v = static_cast<unsigned>(value);
    out.push_back(static_cast<Byte>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<Byte>(v & 0xFFU));
}

inline void appendU24Be(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<Byte>(value & 0xFFU));
}

inline void appendU32Be(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<Byte>(value & 0xFFU));
}

inline void appendU16Le(Bytes& out, std::uint16_t value)
{
    const auto v = static_cast<unsigned>(value);
    out.push_back(static_cast<Byte>(v & 0xFFU));
    out.push_back(static_cast<Byte>((v >> 8U) & 0xFFU));
}

inline void appendU24Le(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
}

inline void appendU32Le(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 24U) & 0xFFU));
}

inline std::uint16_t readU16Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 2 > bytes.size() - offset)
    {
        return 0;
    }
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[offset]) << 8U) |
                                      static_cast<unsigned>(bytes[offset + 1]));
}

inline std::uint32_t readU24Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 3 > bytes.size() - offset)
    {
        return 0;
    }
    return (std::uint32_t(bytes[offset]) << 16U) | (std::uint32_t(bytes[offset + 1]) << 8U) |
           std::uint32_t(bytes[offset + 2]);
}

inline std::uint32_t readU32Be(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 4 > bytes.size() - offset)
    {
        return 0;
    }
    return (std::uint32_t(bytes[offset]) << 24U) | (std::uint32_t(bytes[offset + 1]) << 16U) |
           (std::uint32_t(bytes[offset + 2]) << 8U) | std::uint32_t(bytes[offset + 3]);
}

inline std::uint16_t readU16Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 2 > bytes.size() - offset)
    {
        return 0;
    }
    return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[offset]) |
                                      (static_cast<unsigned>(bytes[offset + 1]) << 8U));
}

inline std::uint32_t readU24Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 3 > bytes.size() - offset)
    {
        return 0;
    }
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8U) |
           (std::uint32_t(bytes[offset + 2]) << 16U);
}

inline std::uint32_t readU32Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset > bytes.size() || 4 > bytes.size() - offset)
    {
        return 0;
    }
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8U) |
           (std::uint32_t(bytes[offset + 2]) << 16U) | (std::uint32_t(bytes[offset + 3]) << 24U);
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
        return offset < bytes.size() ? std::uint32_t(bytes[offset]) : 0U;
    case 2:
        return readU16Be(bytes, offset);
    case 3:
        return readU24Be(bytes, offset);
    case 4:
        return readU32Be(bytes, offset);
    default:
        return 0U;
    }
}

// Least-significant byte first. See readUBe.
inline std::uint32_t readULe(ByteView bytes, std::size_t offset, std::uint32_t width)
{
    switch (width)
    {
    case 1:
        return offset < bytes.size() ? std::uint32_t(bytes[offset]) : 0U;
    case 2:
        return readU16Le(bytes, offset);
    case 3:
        return readU24Le(bytes, offset);
    case 4:
        return readU32Le(bytes, offset);
    default:
        return 0U;
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
    const auto v = static_cast<unsigned>(value);
    out[offset] = static_cast<Byte>((v >> 8U) & 0xFFU);
    out[offset + 1] = static_cast<Byte>(v & 0xFFU);
}

inline void writeU24Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 3 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>((value >> 16U) & 0xFFU);
    out[offset + 1] = static_cast<Byte>((value >> 8U) & 0xFFU);
    out[offset + 2] = static_cast<Byte>(value & 0xFFU);
}

inline void writeU32Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 4 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>((value >> 24U) & 0xFFU);
    out[offset + 1] = static_cast<Byte>((value >> 16U) & 0xFFU);
    out[offset + 2] = static_cast<Byte>((value >> 8U) & 0xFFU);
    out[offset + 3] = static_cast<Byte>(value & 0xFFU);
}

inline void writeU16Le(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset > out.size() || 2 > out.size() - offset)
    {
        return;
    }
    const auto v = static_cast<unsigned>(value);
    out[offset] = static_cast<Byte>(v & 0xFFU);
    out[offset + 1] = static_cast<Byte>((v >> 8U) & 0xFFU);
}

inline void writeU24Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 3 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>(value & 0xFFU);
    out[offset + 1] = static_cast<Byte>((value >> 8U) & 0xFFU);
    out[offset + 2] = static_cast<Byte>((value >> 16U) & 0xFFU);
}

inline void writeU32Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset > out.size() || 4 > out.size() - offset)
    {
        return;
    }
    out[offset] = static_cast<Byte>(value & 0xFFU);
    out[offset + 1] = static_cast<Byte>((value >> 8U) & 0xFFU);
    out[offset + 2] = static_cast<Byte>((value >> 16U) & 0xFFU);
    out[offset + 3] = static_cast<Byte>((value >> 24U) & 0xFFU);
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
    const auto sum = std::accumulate(slice.begin(), slice.end(), 0U);
    return static_cast<Byte>(sum & 0xFFU);
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
    // A Byte always renders as exactly two digits plus the separator, so the
    // buffer is never truncated; appending up to `out` holds either way.
    std::array<char, 3> hex = {};
    for (const Byte byte : bytes)
    {
        const auto rendered = std::format_to_n(hex.data(), hex.size(), "{:02x} ", byte);
        msg.append(hex.data(), rendered.out);
    }
    return msg;
}

} // namespace bytes
