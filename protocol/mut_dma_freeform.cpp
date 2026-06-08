#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_codec.h"
namespace mutdma {
QByteArray buildSetupFrame(quint8 setupCmd, quint8 channelCount) {
    QByteArray payload; payload.append(char(channelCount));
    return buildCommandFrame(setupCmd, payload, TRAILER_FREEFORM);
}
quint8 sizeToDescriptor(quint8 len) {
    switch (len) { case 1: return 0; case 2: return 1; case 4: return 2; default: return 0; }
}
int reqLen(int channelCount) {
    return ((channelCount + 3) >> 2) + channelCount * 2 + 0x1c;
}
QByteArray buildIdListFrame(quint8 listCmd, const QVector<Channel>& channels) {
    const int n = channels.size();
    const int total = reqLen(n);
    QByteArray f(total, char(0));
    f[0] = char(listCmd);
    f[1] = char(quint8(n));
    const int descBytes = (n + 3) >> 2;
    for (int i = 0; i < n; ++i) {                       // pack 2-bit size descriptors, MSB-first
        const quint8 d = sizeToDescriptor(channels.at(i).len) & 0x3;
        const int shift = (3 - (i & 3)) * 2;
        f[2 + (i >> 2)] = char(quint8(f.at(2 + (i >> 2))) | (d << shift));
    }
    int idOff = 2 + descBytes;
    for (int i = 0; i < n; ++i) {                       // big-endian u16 ids
        f[idOff++] = char(quint8(channels.at(i).id >> 8));
        f[idOff++] = char(quint8(channels.at(i).id & 0xFF));
    }
    f[total - 2] = char(sum8(f, 0, total - 2));
    f[total - 1] = char(TRAILER_STD);
    return f;
}
int responseDataLength(const QVector<Channel>& channels) {
    int n = 0; for (const Channel& c : channels) n += c.len; return n;
}
QVector<quint32> decodeStreamValues(const QVector<Channel>& channels, const QByteArray& data) {
    QVector<quint32> out; int off = 0;
    for (const Channel& c : channels) {
        quint32 v = 0;
        for (int k = 0; k < c.len && off < data.size(); ++k, ++off)
            v = (v << 8) | quint8(data.at(off));        // big-endian
        out.append(v);
    }
    return out;
}
}
