#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstdint>

namespace SsmProtocol
{

bytes::Bytes calculateSeedKey(bytes::ByteView seed, const std::uint16_t *keytogenerateindex,
                              const std::uint8_t *indextransformation);
bytes::Bytes calculatePayload(bytes::ByteView buf, std::uint32_t len,
                              const std::uint16_t *keytogenerateindex,
                              const std::uint8_t *indextransformation);
bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId);
bool hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId);
bool hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix,
                      bytes::Byte receiverId, bytes::Byte senderId);

} // namespace SsmProtocol
