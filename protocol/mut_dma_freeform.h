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
}
