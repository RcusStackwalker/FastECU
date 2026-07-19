#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include <QVector>

#include <cstdint>
#include <vector>

#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
namespace mutdma
{
// Max payload bytes per 0x87/3 write frame: 48 payload bytes minus
// [subHi,subLo,addrHi,addrLo,size] = 5 header bytes => 43.
constexpr int MAX_WRITE_CHUNK = 43;
// 0x87 sub-cmd 3 (write arbitrary memory) frames, chunked to MAX_WRITE_CHUNK.
// Each: cmd 0x87, payload [00 03 addrHi addrLo size data...], trailer 0x0D.
// Returns no frames if addr + bytes.size() > 0x10000 (out-of-range write).
std::vector<MutDmaFrame> buildWriteFrames(std::uint16_t addr, bytes::ByteView bytes);
// Build 1-byte free-form channels covering [addr, addr+len). Caller chunks to the
// id-list limit (~43 channels) across multiple streaming cycles.
QVector<Channel> planReadChannels(std::uint16_t addr, int len);
// Concatenate the low byte of each decoded 1-byte channel value into a buffer.
bytes::Bytes reassembleRead(const QVector<std::uint32_t>& values);
} // namespace mutdma
