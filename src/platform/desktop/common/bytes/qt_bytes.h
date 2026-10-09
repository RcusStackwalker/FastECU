#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <QByteArray>
#include <QString>

namespace bytes
{

inline ByteView View(const QByteArray& bytes)
{
    return ByteView(reinterpret_cast<const Byte *>(bytes.constData()), static_cast<std::size_t>(bytes.size()));
}

inline Bytes FromQByteArray(const QByteArray& bytes)
{
    const auto byte_view = View(bytes);
    return Bytes(byte_view.begin(), byte_view.end());
}

inline QByteArray ToQByteArray(ByteView bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

inline MutableByteView MutableView(QByteArray& bytes)
{
    return MutableByteView(reinterpret_cast<Byte *>(bytes.data()), static_cast<std::size_t>(bytes.size()));
}

inline void AppendU16Be(QByteArray& out, std::uint16_t value)
{
    const auto v = static_cast<unsigned>(value);
    out.append(static_cast<char>((v >> 8U) & 0xFFU));
    out.append(static_cast<char>(v & 0xFFU));
}

inline void AppendU24Be(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>((value >> 16U) & 0xFFU));
    out.append(static_cast<char>((value >> 8U) & 0xFFU));
    out.append(static_cast<char>(value & 0xFFU));
}

inline void AppendU32Be(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>((value >> 24U) & 0xFFU));
    out.append(static_cast<char>((value >> 16U) & 0xFFU));
    out.append(static_cast<char>((value >> 8U) & 0xFFU));
    out.append(static_cast<char>(value & 0xFFU));
}

inline void AppendU16Le(QByteArray& out, std::uint16_t value)
{
    const auto v = static_cast<unsigned>(value);
    out.append(static_cast<char>(v & 0xFFU));
    out.append(static_cast<char>((v >> 8U) & 0xFFU));
}

inline void AppendU24Le(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFU));
    out.append(static_cast<char>((value >> 8U) & 0xFFU));
    out.append(static_cast<char>((value >> 16U) & 0xFFU));
}

inline void AppendU32Le(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFU));
    out.append(static_cast<char>((value >> 8U) & 0xFFU));
    out.append(static_cast<char>((value >> 16U) & 0xFFU));
    out.append(static_cast<char>((value >> 24U) & 0xFFU));
}

inline void WriteU16Be(QByteArray& out, std::size_t offset, std::uint16_t value)
{
    WriteU16Be(MutableView(out), offset, value);
}

inline void WriteU24Be(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    WriteU24Be(MutableView(out), offset, value);
}

inline void WriteU32Be(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    WriteU32Be(MutableView(out), offset, value);
}

inline void WriteU16Le(QByteArray& out, std::size_t offset, std::uint16_t value)
{
    WriteU16Le(MutableView(out), offset, value);
}

inline void WriteU24Le(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    WriteU24Le(MutableView(out), offset, value);
}

inline void WriteU32Le(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    WriteU32Le(MutableView(out), offset, value);
}

inline QString ToHex(const QByteArray& data)
{
    return QString::fromStdString(ToHex(View(data)));
}

} // namespace bytes
