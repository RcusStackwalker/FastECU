#pragma once
#include <QByteArray>
namespace mutdma {
constexpr int   FRAME_LEN       = 51;
constexpr quint8 TRAILER_STD      = 0x0D;
constexpr quint8 TRAILER_FREEFORM = 0x0A;
constexpr int   CHECKSUM_OFFSET = 49;   // sum8 of bytes [0..48]
constexpr int   TRAILER_OFFSET  = 50;
// 8-bit sum of len bytes starting at `from`.
quint8 sum8(const QByteArray& bytes, int from, int len);
// 51-byte frame: [cmd][payload, zero-padded to fill 0..48][sum8(0..48)][trailer].
// payload longer than 48 bytes is truncated.
QByteArray buildCommandFrame(quint8 cmd, const QByteArray& payload, quint8 trailer);
// True iff size==51, byte49==sum8(0..48), byte50 in {0x0D,0x0A}.
bool verifyFrame(const QByteArray& frame);
}
