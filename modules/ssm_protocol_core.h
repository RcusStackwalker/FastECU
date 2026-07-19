#ifndef SSM_PROTOCOL_CORE_H
#define SSM_PROTOCOL_CORE_H

#include "src/algorithms/protocol/bytes.h"

#include <cstdint>
#include <string>

namespace SsmProtocol
{

bytes::Bytes calculateSeedKey(bytes::ByteView seed, const std::uint16_t *keytogenerateindex,
                              const std::uint8_t *indextransformation);
bytes::Bytes calculatePayload(bytes::ByteView buf, std::uint32_t len,
                              const std::uint16_t *keytogenerateindex,
                              const std::uint8_t *indextransformation);
bytes::Byte checksum(bytes::ByteView output, bool dec0x100 = false);
bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId,
                       bool dec0x100 = false);
bool hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId,
                   bool dec0x100 = false);
bool hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix,
                      bytes::Byte receiverId, bytes::Byte senderId,
                      bool dec0x100 = false);
std::string toHex(bytes::ByteView received);
std::uint32_t crc32(bytes::ByteView bytes);

} // namespace SsmProtocol

#endif // SSM_PROTOCOL_CORE_H
