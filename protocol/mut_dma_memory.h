#pragma once
#include <QByteArray>
#include <QVector>
namespace mutdma {
// Max payload bytes per 0x87/3 write frame: 48 payload bytes minus
// [subHi,subLo,addrHi,addrLo,size] = 5 header bytes => 43.
constexpr int MAX_WRITE_CHUNK = 43;
// 0x87 sub-cmd 3 (write arbitrary memory) frames, chunked to MAX_WRITE_CHUNK.
// Each: cmd 0x87, payload [00 03 addrHi addrLo size data...], trailer 0x0D.
QVector<QByteArray> buildWriteFrames(quint16 addr, const QByteArray& bytes);
}
