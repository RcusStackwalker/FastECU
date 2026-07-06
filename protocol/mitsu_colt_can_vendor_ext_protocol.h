#pragma once
#include <QByteArray>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) vendor diagnostic-extension
// challenge algorithm. Reverse-engineered by static disassembly of a
// commercially-tuned ROM's diagnostic-dispatcher hijack (not from any
// vendor source or documentation) — see
// docs/superpowers/specs/2026-07-04-mitsu-colt-can-vendor-ext-challenge-design.md
// for the full derivation.
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

constexpr quint8 kServiceSecurityAccess = 0x27;
constexpr quint8 kVendorChallengeSeedSubfunction = 0x41; // ASCII 'A'
constexpr quint8 kVendorChallengeKeySubfunction = 0x42;  // ASCII 'B'

// Forward transform, ported verbatim from ROM 47110032 offset 0x510b8. This
// is what the ECU applies to its internal secret to produce the seed value
// it sends the client. Provided for documentation/completeness and as the
// basis for the round-trip check in tests — not what a client calls.
quint32 challengeTransform(quint32 secret);

// Inverse of challengeTransform(), analytically derived (see design doc
// for the derivation) and verified against the forward function across the
// full 32-bit domain. THIS is what a real client calls: given the 4-byte
// seed the ECU sends, computes the key value the ECU will accept.
quint32 challengeInverseTransform(quint32 seed);

// Big-endian byte-array convenience wrappers matching the 4-byte wire
// layout, mirroring MitsuColtCan::seedKey's shape.
quint32 bytesToSeed(const QByteArray &seedBytes);  // expects exactly 4 bytes
QByteArray keyToBytes(quint32 key);                 // produces exactly 4 bytes

// SID 0x27/'A' (vendor seed request): [SID][0x41].
QByteArray buildChallengeSeedRequestFrame();

// SID 0x27/'B' (vendor key answer): [SID][0x42][4-byte key].
QByteArray buildChallengeKeyFrame(quint32 key);

} // namespace MitsuColtCanVendorExt
