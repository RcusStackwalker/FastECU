#pragma once
#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <cstdint>
#include <vector>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) Cdbg live-data logging protocol.
// Ported from externals/livemonitor/cdbgengine.cpp + logitemsmodel.cpp.
// Request CAN ID 0x630, reply CAN ID 0x631. Raw CAN (not ISO-TP): every
// command and reply is a single 8-byte frame.
namespace MitsuColtCanCdbg
{

constexpr std::uint32_t kRequestCanId = 0x630;
constexpr std::uint32_t kReplyCanId = 0x631;

constexpr bytes::Byte kCmdInit = 1;
constexpr bytes::Byte kCmdSecuritySeed = 18;
constexpr bytes::Byte kCmdSecurityKey = 19;
constexpr bytes::Byte kCmdLogReset = 20;
constexpr bytes::Byte kCmdLogSelectItem = 21;
constexpr bytes::Byte kCmdLogSetPointer = 22;
constexpr bytes::Byte kCmdLogStart = 6;

constexpr bytes::Byte kSecurityLogAccess = 2;

// Every streamed reply frame reserves byte 0 as a frame-index marker, so a
// frame carries at most 7 payload bytes. getLogStartCommand's frameCount
// field matches cdbgengine.cpp's Q_ASSERT(frameCount <= 8): 8 frame slots.
constexpr int kMaxFrameBytes = 7;
constexpr int kMaxFrames = 8;

using CdbgFrame = std::array<bytes::Byte, 8>;

// One logged RAM value: address + element size (1, 2, or 4 bytes).
struct CdbgChannel
{
    std::uint32_t pointer;
    bytes::Byte size;
};

// {1, 1, 0,0,0,0,0,0}
CdbgFrame buildInitFrame();

// {18, 0, 2, 0,0,0,0,0} - kSecurityLogAccess seed request.
CdbgFrame buildSecuritySeedRequestFrame();

// Ported byte-for-byte from cdbgengine.cpp's seed_to_key(): per-byte
// increment (keyed on the byte's low 2 bits) + 8-bit left-rotate by 3,
// then a parity-keyed byte permutation, then two 16-bit multiply-accumulates.
// Pure integer math - no ROM-specific addresses involved.
std::uint32_t seedToKey(std::uint32_t seed);

// Extracts the big-endian 4-byte seed from the security-seed-request reply
// (reply bytes [4,8)). Returns 0 if reply is shorter than 8 bytes.
std::uint32_t extractSeed(bytes::ByteView reply);

// {19, 0, key[0..3] (big-endian), 0, 0}
CdbgFrame buildSecurityKeyFrame(std::uint32_t key);

// True if the key-response reply grants access (byte[3] != 0) - matches
// livemonitor's security_flags check (the only documented success signal
// for this step, in mainwindow.cpp's dead-code cdbgSecurityAccessCallback).
// Returns false if reply is shorter than 4 bytes.
bool securityGranted(bytes::ByteView reply);

// {20, 0, instance, 0, 0, 0, 0x06, 0x31}
CdbgFrame buildLogResetFrame(bytes::Byte instance);

// {6, 0, 1, instance, frameCount, intervalUnit, intervalHi, intervalLo}.
// intervalMs is encoded directly in milliseconds if it fits in 16 bits
// (intervalUnit=0), otherwise in tens-of-milliseconds (intervalUnit=1,
// truncating division) - matches getLogStartCommand.
CdbgFrame buildLogStartFrame(bytes::Byte instance, bytes::Byte frameCount, std::uint32_t intervalMs);

// Packs channels into frames of at most kMaxFrameBytes total size each (byte
// 0 of every streamed frame is the frame-index marker, so payload starts at
// byte 1), at most kMaxFrames frames, matching
// LogItemsModel::loadSchema's byteIndex/frameIndex bookkeeping. Returns
// false (outFrames untouched) if channels is empty or does not fit within
// kMaxFrames frames.
bool batchChannelsIntoFrames(const std::vector<CdbgChannel>& channels,
                             std::vector<std::vector<CdbgChannel>>& outFrames);

// Builds the {21,...}/{22,...} command pairs (in order: select0, pointer0,
// select1, pointer1, ...) that configure one frame's items, matching
// getLogFrameInitCommands. Caller must have already produced frameItems via
// batchChannelsIntoFrames.
std::vector<CdbgFrame> buildFrameInitFrames(bytes::Byte instance, bytes::Byte frameIndex,
                                            const std::vector<CdbgChannel>& frameItems);

// Decodes one streamed reply frame (byte 0 = frame index, bytes [1, N) =
// concatenated big-endian channel values per frameItems, in order) into one
// raw unsigned value per channel. Returns an empty vector if frame is
// shorter than 1 byte, frame[0] doesn't match expectedFrameIndex, or frame
// is too short to hold every channel in frameItems.
std::vector<std::uint32_t> decodeFrame(bytes::Byte expectedFrameIndex,
                                       const std::vector<CdbgChannel>& frameItems,
                                       bytes::ByteView frame);

} // namespace MitsuColtCanCdbg
