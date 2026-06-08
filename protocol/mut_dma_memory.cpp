#include "protocol/mut_dma_memory.h"
#include "protocol/mut_dma_codec.h"
namespace mutdma {
QVector<QByteArray> buildWriteFrames(quint16 addr, const QByteArray& bytes) {
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
}
