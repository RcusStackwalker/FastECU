#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <cstddef>

namespace mutdma
{
constexpr int kFrameLen = 51;
constexpr bytes::Byte kTrailerStd = 0x0D;
constexpr bytes::Byte kTrailerFreeform = 0x0A;
constexpr int kChecksumOffset = 49; // sum8 of bytes [0..48]
constexpr int kTrailerOffset = 50;

using MutDmaFrame = std::array<bytes::Byte, kFrameLen>;

// 8-bit sum of len bytes starting at `from`.
bytes::Byte sum8(bytes::ByteView bytes, std::size_t from, std::size_t len);
bytes::Byte sum8(bytes::ByteView bytes);

// 51-byte frame: [cmd][payload, zero-padded to fill 0..48][sum8(0..48)][trailer].
// payload longer than 48 bytes is truncated.
MutDmaFrame buildCommandFrame(bytes::Byte cmd, bytes::ByteView payload, bytes::Byte trailer);
// True iff size==51, byte49==sum8(0..48), byte50 in {0x0D,0x0A}.
bool verifyFrame(bytes::ByteView frame);

struct StreamFrame
{
    bytes::Byte log_id = 0;
    bytes::Bytes data;
    bool ok = false;
};
// Variable-length streamed frame: [logId][data...][sum8(all but last 2)][0x0D].
// data = bytes between logId and the checksum.
StreamFrame parseStreamFrame(bytes::ByteView frame);
} // namespace mutdma
