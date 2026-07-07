#ifndef FASTECU_QT_BYTES_H
#define FASTECU_QT_BYTES_H

#include "protocol/bytes.h"

#include <QByteArray>

namespace bytes {

inline ByteView view(const QByteArray &bytes)
{
    return ByteView(reinterpret_cast<const Byte *>(bytes.constData()),
                    static_cast<std::size_t>(bytes.size()));
}

inline Bytes fromQByteArray(const QByteArray &bytes)
{
    const auto byteView = view(bytes);
    return Bytes(byteView.begin(), byteView.end());
}

inline QByteArray toQByteArray(ByteView bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

} // namespace bytes

#endif // FASTECU_QT_BYTES_H
