#include "protocol/mut_dma_memory.h"

#include <algorithm>

namespace mutdma {
std::vector<MutDmaFrame> buildWriteFrames(std::uint16_t addr, bytes::ByteView bytes) {
    if (std::uint32_t(addr) + std::uint32_t(bytes.size()) > 0x10000u) return {};
    std::vector<MutDmaFrame> frames;
    std::size_t off = 0;
    while (off < bytes.size()) {
        const std::size_t chunk = std::min(static_cast<std::size_t>(MAX_WRITE_CHUNK), bytes.size() - off);
        const std::uint16_t a = static_cast<std::uint16_t>(addr + off);
        bytes::Bytes payload;
        payload.reserve(5 + chunk);
        payload.push_back(0x00);                 // sub-selector hi
        payload.push_back(0x03);                 // sub-selector lo = write arbitrary
        bytes::appendU16Be(payload, a);
        payload.push_back(static_cast<bytes::Byte>(chunk));
        payload.insert(payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(off),
                       bytes.begin() + static_cast<std::ptrdiff_t>(off + chunk));
        frames.push_back(buildCommandFrame(0x87, payload, TRAILER_STD));
        off += chunk;
    }
    return frames;
}
QVector<Channel> planReadChannels(std::uint16_t addr, int len) {
    QVector<Channel> ch;
    for (int i = 0; i < len; ++i) ch.append(Channel{ static_cast<std::uint16_t>(addr + i), 1 });
    return ch;
}
bytes::Bytes reassembleRead(const QVector<std::uint32_t>& values) {
    bytes::Bytes out;
    out.reserve(static_cast<std::size_t>(values.size()));
    for (std::uint32_t v : values) out.push_back(static_cast<bytes::Byte>(v & 0xFF));
    return out;
}
}
