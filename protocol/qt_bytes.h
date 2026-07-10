#ifndef FASTECU_QT_BYTES_H
#define FASTECU_QT_BYTES_H

#include "protocol/bytes.h"

#include <QByteArray>

namespace bytes
{

inline ByteView view(const QByteArray& bytes)
{
    return ByteView(reinterpret_cast<const Byte *>(bytes.constData()),
                    static_cast<std::size_t>(bytes.size()));
}

inline Bytes fromQByteArray(const QByteArray& bytes)
{
    const auto byteView = view(bytes);
    return Bytes(byteView.begin(), byteView.end());
}

inline QByteArray toQByteArray(ByteView bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

inline MutableByteView mutableView(QByteArray& bytes)
{
    return MutableByteView(reinterpret_cast<Byte *>(bytes.data()),
                           static_cast<std::size_t>(bytes.size()));
}

inline void appendU16Be(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void appendU24Be(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void appendU32Be(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void appendU16Le(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
}

inline void appendU24Le(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
}

inline void appendU32Le(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 24) & 0xFF));
}

inline void writeU16Be(QByteArray& out, std::size_t offset, std::uint16_t value)
{
    writeU16Be(mutableView(out), offset, value);
}

inline void writeU24Be(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU24Be(mutableView(out), offset, value);
}

inline void writeU32Be(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU32Be(mutableView(out), offset, value);
}

inline void writeU16Le(QByteArray& out, std::size_t offset, std::uint16_t value)
{
    writeU16Le(mutableView(out), offset, value);
}

inline void writeU24Le(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU24Le(mutableView(out), offset, value);
}

inline void writeU32Le(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU32Le(mutableView(out), offset, value);
}

} // namespace bytes

#endif // FASTECU_QT_BYTES_H
