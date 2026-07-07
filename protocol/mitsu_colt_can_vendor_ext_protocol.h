#pragma once
#include "protocol/bytes.h"

#include <QByteArray>

#include <cstdint>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) vendor diagnostic-extension
// challenge algorithm. Reverse-engineered by static disassembly of a
// commercially-tuned ROM's diagnostic-dispatcher hijack (not from any
// vendor source or documentation).
//
// This challenge gates entry into the 0x85 ("bootload") diagnostic session
// on ROMs carrying this specific third-party extension. It is unrelated to
// MitsuColtCan::seedKey (the ECU's own factory SecurityAccess) and to
// MitsuColtCanCdbg (livemonitor's separate live-logging protocol, CAN
// 0x630/0x631) — only ROMs with this vendor extension present use it; stock
// ROMs and this project's own patches never touch it.
//
// Pure, hardware-independent functions only — no I/O here.
namespace MitsuColtCanVendorExt {

constexpr bytes::Byte kServiceReadMemoryByAddress = 0x23;
constexpr bytes::Byte kVendorChallengeSelector = 0x27;
constexpr bytes::Byte kVendorChallengeSeedSubfunction = 0x41; // ASCII 'A'
constexpr bytes::Byte kVendorChallengeKeySubfunction = 0x42;  // ASCII 'B'

// Forward transform, ported verbatim from ROM 47110032 offset 0x510b8. This
// is what the ECU applies to its internal secret to produce the seed value
// it sends the client. Provided for documentation/completeness and as the
// basis for the round-trip check in tests — not what a client calls.
std::uint32_t challengeTransform(std::uint32_t secret);

// Inverse of challengeTransform(), analytically derived and verified against
// the forward function across the full 32-bit domain. THIS is what a real
// client calls: given the 4-byte seed the ECU sends, computes the key value
// the ECU will accept.
std::uint32_t challengeInverseTransform(std::uint32_t seed);

std::uint32_t bytesToSeed(bytes::ByteView seedBytes); // expects exactly 4 bytes
bytes::Bytes keyBytes(std::uint32_t key);             // produces exactly 4 bytes

// SID 0x23 vendor extension seed request: [0x23][0x27][0x41].
bytes::Bytes buildChallengeSeedRequest();

// SID 0x23 vendor extension key answer: [0x23][0x27][0x42][4-byte key].
bytes::Bytes buildChallengeKey(std::uint32_t key);

// Big-endian byte-array convenience wrappers matching the 4-byte wire
// layout, mirroring MitsuColtCan::seedKey's shape.
std::uint32_t bytesToSeed(const QByteArray &seedBytes); // expects exactly 4 bytes
QByteArray keyToBytes(std::uint32_t key);               // produces exactly 4 bytes

QByteArray buildChallengeSeedRequestFrame();

QByteArray buildChallengeKeyFrame(std::uint32_t key);

} // namespace MitsuColtCanVendorExt
