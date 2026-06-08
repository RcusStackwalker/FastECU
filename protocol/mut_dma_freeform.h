#pragma once
#include <QByteArray>
#include <QVector>
namespace mutdma {
struct Channel {
    quint16 id;    // PID or RAM address (mapped 0x4000-0xBFFF direct / 0x8000-window -> 0x800000+addr)
    quint8  len;   // element size in bytes: 1, 2, or 4
};
// Setup frame: [setupCmd (0xA0/0xB0)][channelCount][pad][sum8][0x0A], 51 bytes.
QByteArray buildSetupFrame(quint8 setupCmd, quint8 channelCount);
// 0 for 1 byte, 1 for 2 bytes, 2 for 4 bytes. Returns 0 for unexpected sizes.
quint8 sizeToDescriptor(quint8 len);
// reqLen = ((N+3)>>2) + N*2 + 0x1c   (header+descriptors+ids+overhead+csum+trailer)
int reqLen(int channelCount);
// Id-list (host reply to ACK-1): [listCmd 0xA1..0xA4][N][2-bit size descriptors,
// ceil(N/4) bytes, channel i at bits[(3-(i%4))*2]][N x u16 ids big-endian][zero pad]
// [sum8(0..len-3)][0x0D]. Total length == reqLen(N). listCmd selects the rate slot.
QByteArray buildIdListFrame(quint8 listCmd, const QVector<Channel>& channels);
// Sum of element sizes = number of data bytes a stream frame carries for these channels.
int responseDataLength(const QVector<Channel>& channels);
// Decode the stream data payload into one big-endian value per channel, in order.
QVector<quint32> decodeStreamValues(const QVector<Channel>& channels, const QByteArray& data);
}
