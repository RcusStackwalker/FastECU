#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <cstdint>

namespace SsmProtocol
{

using SeedKeyToGenerateIndex = std::span<const std::uint16_t, 16>;
using KeyToGenerateIndex = std::span<const std::uint16_t, 4>;
using IndexTransformation = std::span<const std::uint8_t, 32>;

// The index transformation calculateSeedKey and calculatePayload take.
//
// This is an SsmProtocol-level constant, not a per-family one: the same 32
// entries drive every Subaru seed-key and payload transform in the tree. It
// was previously spelled out as an inline 32-entry literal at fourteen
// production call sites across src/backend/flash/ecu and
// src/backend/flash/eeprom.
//
// The KEY-TO-GENERATE-INDEX tables those calls pass alongside it are genuinely
// per-family protocol data and stay at their call sites. Only the
// transformation is shared.
inline constexpr std::array<std::uint8_t, 32> kIndexTransformationStock{
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

// The ECUTEK-reflashed variant, used by the Denso SH705x EEPROM pair's
// ECUTEK seed-key branches.
//
// It differs from kIndexTransformationStock in exactly its first five entries
// (0x4, 0x2, 0x5, 0x1, 0x8 against 0x5, 0x6, 0x7, 0x1, 0x9) and is identical
// in the remaining twenty-seven. Naming the two is the point: as inline
// literals the difference was twelve hex digits apart in a wall of thirty-two
// and could only be found by reading a comment that said so.
inline constexpr std::array<std::uint8_t, 32> kIndexTransformationEcutek{
    0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes calculateSeedKey(bytes::ByteView seed, SeedKeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation);
bytes::Bytes calculatePayload(bytes::ByteView buf, std::uint32_t len, KeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation);
bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId);
bool hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId);
bool hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix, bytes::Byte receiverId, bytes::Byte senderId);

} // namespace SsmProtocol
