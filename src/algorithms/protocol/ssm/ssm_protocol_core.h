#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstdint>

namespace SsmProtocol
{

using SeedKeyToGenerateIndex = std::span<const std::uint16_t, 16>;
using KeyToGenerateIndex = std::span<const std::uint16_t, 4>;
using IndexTransformation = std::span<const std::uint8_t, 32>;

bytes::Bytes calculateSeedKey(bytes::ByteView seed, SeedKeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation);
bytes::Bytes calculatePayload(bytes::ByteView buf, std::uint32_t len,
                              KeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation);
bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId);
bool hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId);
bool hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix,
                      bytes::Byte receiverId, bytes::Byte senderId);

} // namespace SsmProtocol
