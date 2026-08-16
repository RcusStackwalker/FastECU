#include "src/algorithms/protocol/ssm/ssm_protocol.h"

#include "src/algorithms/protocol/qt_bytes.h"

namespace SsmProtocol
{

QByteArray calculateSeedKey(const QByteArray& seed, SeedKeyToGenerateIndex keytogenerateindex,
                            IndexTransformation indextransformation)
{
    return bytes::toQByteArray(calculateSeedKey(bytes::view(seed), keytogenerateindex, indextransformation));
}

QByteArray calculatePayload(const QByteArray& buf, uint32_t len, KeyToGenerateIndex keytogenerateindex,
                            IndexTransformation indextransformation)
{
    return bytes::toQByteArray(calculatePayload(bytes::view(buf), len, keytogenerateindex, indextransformation));
}

QByteArray addHeader(const QByteArray& output, uint8_t testerId, uint8_t targetId)
{
    return bytes::toQByteArray(addHeader(bytes::view(output), testerId, targetId));
}

bool hasValidFrame(const QByteArray& frame, uint8_t receiverId, uint8_t senderId)
{
    return hasValidFrame(bytes::view(frame), receiverId, senderId);
}

bool hasPayloadPrefix(const QByteArray& frame, const QByteArray& prefix, uint8_t receiverId, uint8_t senderId)
{
    return hasPayloadPrefix(bytes::view(frame), bytes::view(prefix), receiverId, senderId);
}

} // namespace SsmProtocol
