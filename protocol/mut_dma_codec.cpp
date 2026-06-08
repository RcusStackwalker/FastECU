#include "protocol/mut_dma_codec.h"
namespace mutdma {
quint8 sum8(const QByteArray& bytes, int from, int len) {
    quint32 s = 0;
    for (int i = from; i < from + len && i < bytes.size(); ++i)
        s += quint8(bytes.at(i));
    return quint8(s & 0xFF);
}
QByteArray buildCommandFrame(quint8 cmd, const QByteArray& payload, quint8 trailer) {
    QByteArray f(FRAME_LEN, char(0));
    f[0] = char(cmd);
    const int n = qMin(payload.size(), CHECKSUM_OFFSET - 1); // bytes 1..48 = 48 max
    for (int i = 0; i < n; ++i) f[1 + i] = payload.at(i);
    f[CHECKSUM_OFFSET] = char(sum8(f, 0, CHECKSUM_OFFSET));
    f[TRAILER_OFFSET]  = char(trailer);
    return f;
}
bool verifyFrame(const QByteArray& frame) {
    if (frame.size() != FRAME_LEN) return false;
    if (quint8(frame.at(CHECKSUM_OFFSET)) != sum8(frame, 0, CHECKSUM_OFFSET)) return false;
    const quint8 t = quint8(frame.at(TRAILER_OFFSET));
    return t == TRAILER_STD || t == TRAILER_FREEFORM;
}
StreamFrame parseStreamFrame(const QByteArray& frame) {
    StreamFrame s;
    if (frame.size() < 3) return s;                       // id + csum + trailer minimum
    if (quint8(frame.at(frame.size()-1)) != TRAILER_STD) return s;
    const int csumIdx = frame.size() - 2;
    if (quint8(frame.at(csumIdx)) != sum8(frame, 0, csumIdx)) return s;
    s.logId = quint8(frame.at(0));
    s.data  = frame.mid(1, csumIdx - 1);
    s.ok = true;
    return s;
}
}
