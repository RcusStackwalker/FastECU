#include "src/algorithms/protocol/ssm/ssm_protocol.h"

#include "src/algorithms/protocol/qt_bytes.h"

namespace SsmProtocol
{

QByteArray calculateSeedKey(const QByteArray& seed, const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation)
{
    return bytes::toQByteArray(calculateSeedKey(bytes::view(seed), keytogenerateindex,
                                                indextransformation));
}

QByteArray calculatePayload(const QByteArray& buf, uint32_t len,
                            const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation)
{
    return bytes::toQByteArray(calculatePayload(bytes::view(buf), len, keytogenerateindex,
                                                indextransformation));
}

uint8_t checksum(const QByteArray& output, bool dec0x100)
{
    return checksum(bytes::view(output), dec0x100);
}

QByteArray addHeader(const QByteArray& output, uint8_t testerId, uint8_t targetId, bool dec0x100)
{
    return bytes::toQByteArray(addHeader(bytes::view(output), testerId, targetId, dec0x100));
}

bool hasValidFrame(const QByteArray& frame, uint8_t receiverId, uint8_t senderId, bool dec0x100)
{
    return hasValidFrame(bytes::view(frame), receiverId, senderId, dec0x100);
}

bool hasPayloadPrefix(const QByteArray& frame, const QByteArray& prefix,
                      uint8_t receiverId, uint8_t senderId, bool dec0x100)
{
    return hasPayloadPrefix(bytes::view(frame), bytes::view(prefix), receiverId, senderId,
                            dec0x100);
}

QString toHex(const QByteArray& received)
{
    return QString::fromStdString(toHex(bytes::view(received)));
}

uint32_t crc32(const unsigned char *buf, uint32_t len)
{
    if (buf == nullptr)
    {
        return 0;
    }

    return crc32(bytes::ByteView(reinterpret_cast<const bytes::Byte *>(buf), len));
}

} // namespace SsmProtocol
