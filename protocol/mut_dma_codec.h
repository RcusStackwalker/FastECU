#ifndef FASTECU_MUT_DMA_CODEC_H
#define FASTECU_MUT_DMA_CODEC_H

#include "protocol/bytes.h"

#include <array>
#include <cstddef>

namespace mutdma {
constexpr int   FRAME_LEN       = 51;
constexpr bytes::Byte TRAILER_STD      = 0x0D;
constexpr bytes::Byte TRAILER_FREEFORM = 0x0A;
constexpr int   CHECKSUM_OFFSET = 49;   // sum8 of bytes [0..48]
constexpr int   TRAILER_OFFSET  = 50;

using MutDmaFrame = std::array<bytes::Byte, FRAME_LEN>;

// 8-bit sum of len bytes starting at `from`.
bytes::Byte sum8(bytes::ByteView bytes, std::size_t from, std::size_t len);
bytes::Byte sum8(bytes::ByteView bytes);

// 51-byte frame: [cmd][payload, zero-padded to fill 0..48][sum8(0..48)][trailer].
// payload longer than 48 bytes is truncated.
MutDmaFrame buildCommandFrame(bytes::Byte cmd, bytes::ByteView payload, bytes::Byte trailer);
// True iff size==51, byte49==sum8(0..48), byte50 in {0x0D,0x0A}.
bool verifyFrame(bytes::ByteView frame);

struct StreamFrame { bytes::Byte logId = 0; bytes::Bytes data; bool ok = false; };
// Variable-length streamed frame: [logId][data...][sum8(all but last 2)][0x0D].
// data = bytes between logId and the checksum.
StreamFrame parseStreamFrame(bytes::ByteView frame);
}

#endif // FASTECU_MUT_DMA_CODEC_H
