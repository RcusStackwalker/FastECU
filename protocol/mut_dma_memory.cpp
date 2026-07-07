#include "protocol/mut_dma_memory.h"

#include <algorithm>

namespace mutdma {
std::vector<MutDmaFrame> buildWriteFrames(quint16 addr, bytes::ByteView bytes) {
    if (quint32(addr) + quint32(bytes.size()) > 0x10000u) return {};
    std::vector<MutDmaFrame> frames;
    std::size_t off = 0;
    while (off < bytes.size()) {
        const std::size_t chunk = std::min(static_cast<std::size_t>(MAX_WRITE_CHUNK), bytes.size() - off);
        const quint16 a = quint16(addr + off);
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
QVector<Channel> planReadChannels(quint16 addr, int len) {
    QVector<Channel> ch;
    for (int i = 0; i < len; ++i) ch.append(Channel{ quint16(addr + i), 1 });
    return ch;
}
bytes::Bytes reassembleRead(const QVector<quint32>& values) {
    bytes::Bytes out;
    out.reserve(static_cast<std::size_t>(values.size()));
    for (quint32 v : values) out.push_back(static_cast<bytes::Byte>(v & 0xFF));
    return out;
}
}
