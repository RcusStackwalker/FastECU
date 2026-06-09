#include "protocol/mut_dma_memory.h"
#include "protocol/mut_dma_codec.h"
namespace mutdma {
QVector<QByteArray> buildWriteFrames(quint16 addr, const QByteArray& bytes) {
    if (quint32(addr) + quint32(bytes.size()) > 0x10000u) return {};
    QVector<QByteArray> frames;
    int off = 0;
    while (off < bytes.size()) {
        const int chunk = qMin(MAX_WRITE_CHUNK, bytes.size() - off);
        const quint16 a = quint16(addr + off);
        QByteArray payload;
        payload.append(char(0x00));                 // sub-selector hi
        payload.append(char(0x03));                 // sub-selector lo = write arbitrary
        payload.append(char(quint8(a >> 8)));
        payload.append(char(quint8(a & 0xFF)));
        payload.append(char(quint8(chunk)));
        payload.append(bytes.mid(off, chunk));
        frames.append(buildCommandFrame(0x87, payload, TRAILER_STD));
        off += chunk;
    }
    return frames;
}
QVector<Channel> planReadChannels(quint16 addr, int len) {
    QVector<Channel> ch;
    for (int i = 0; i < len; ++i) ch.append(Channel{ quint16(addr + i), 1 });
    return ch;
}
QByteArray reassembleRead(const QVector<quint32>& values) {
    QByteArray out;
    for (quint32 v : values) out.append(char(quint8(v & 0xFF)));
    return out;
}
}
