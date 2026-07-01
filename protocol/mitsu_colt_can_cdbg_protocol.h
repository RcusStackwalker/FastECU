#pragma once
#include <QByteArray>
#include <QVector>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) Cdbg live-data logging protocol.
// Ported from externals/livemonitor/cdbgengine.cpp + logitemsmodel.cpp.
// Request CAN ID 0x630, reply CAN ID 0x631. Raw CAN (not ISO-TP): every
// command and reply is a single 8-byte frame.
namespace MitsuColtCanCdbg {

constexpr quint32 kRequestCanId = 0x630;
constexpr quint32 kReplyCanId   = 0x631;

constexpr quint8 kCmdInit          = 1;
constexpr quint8 kCmdSecuritySeed  = 18;
constexpr quint8 kCmdSecurityKey   = 19;
constexpr quint8 kCmdLogReset      = 20;
constexpr quint8 kCmdLogSelectItem = 21;
constexpr quint8 kCmdLogSetPointer = 22;
constexpr quint8 kCmdLogStart      = 6;

constexpr quint8 kSecurityLogAccess = 2;

// Every streamed reply frame reserves byte 0 as a frame-index marker, so a
// frame carries at most 7 payload bytes. getLogStartCommand's frameCount
// field matches cdbgengine.cpp's Q_ASSERT(frameCount <= 8): 8 frame slots.
constexpr int kMaxFrameBytes = 7;
constexpr int kMaxFrames     = 8;

// One logged RAM value: address + element size (1, 2, or 4 bytes).
struct CdbgChannel {
    quint32 pointer;
    quint8 size;
};

// {1, 1, 0,0,0,0,0,0}
QByteArray buildInitFrame();

// {18, 0, 2, 0,0,0,0,0} - kSecurityLogAccess seed request.
QByteArray buildSecuritySeedRequestFrame();

// Ported byte-for-byte from cdbgengine.cpp's seed_to_key(): per-byte
// increment (keyed on the byte's low 2 bits) + 8-bit left-rotate by 3,
// then a parity-keyed byte permutation, then two 16-bit multiply-accumulates.
// Pure integer math - no ROM-specific addresses involved.
quint32 seedToKey(quint32 seed);

// Extracts the big-endian 4-byte seed from the security-seed-request reply
// (reply bytes [4,8)). Returns 0 if reply is shorter than 8 bytes.
quint32 extractSeed(const QByteArray &reply);

// {19, 0, key[0..3] (big-endian), 0, 0}
QByteArray buildSecurityKeyFrame(quint32 key);

// True if the key-response reply grants access (byte[3] != 0) - matches
// livemonitor's security_flags check (the only documented success signal
// for this step, in mainwindow.cpp's dead-code cdbgSecurityAccessCallback).
// Returns false if reply is shorter than 4 bytes.
bool securityGranted(const QByteArray &reply);

// {20, 0, instance, 0, 0, 0, 0x06, 0x31}
QByteArray buildLogResetFrame(quint8 instance);

// {6, 0, 1, instance, frameCount, intervalUnit, intervalHi, intervalLo}.
// intervalMs is encoded directly in milliseconds if it fits in 16 bits
// (intervalUnit=0), otherwise in tens-of-milliseconds (intervalUnit=1,
// truncating division) - matches getLogStartCommand.
QByteArray buildLogStartFrame(quint8 instance, quint8 frameCount, quint32 intervalMs);

} // namespace MitsuColtCanCdbg
