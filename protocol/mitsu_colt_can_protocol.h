#pragma once
#include <QByteArray>
#include <QVector>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash protocol.
// Ported from externals/livemonitor/obdengine.cpp + colt_flasher.xml.
// Request CAN ID 0x7E0, reply CAN ID 0x7E8 (ISO 15765 / UDS-style OBD).
// Pure, hardware-independent frame builders only — no I/O here.
namespace MitsuColtCan {

constexpr quint8 kServiceDiagnosticSession    = 0x10;
constexpr quint8 kServiceSecurityAccess       = 0x27;
constexpr quint8 kServiceReadMemoryByAddress  = 0x23;
constexpr quint8 kServiceRequestDownload      = 0x34;
constexpr quint8 kServiceTransferData         = 0x36;
constexpr quint8 kServiceRoutineControl       = 0x31;
constexpr quint8 kServiceRequestReflash       = 0x3B;

constexpr quint8 kSessionBasic    = 0x81;
constexpr quint8 kSessionBootload = 0x85;

constexpr quint32 kCrcTransferAddress = 0x200000;
constexpr quint32 kCrcTransferSize    = 2;
constexpr quint8  kRoutineCheckCrc    = 225;
constexpr quint8  kRoutineErase       = 224;

constexpr quint32 kUserspaceStart      = 0x008000;
constexpr quint32 kUserspaceEnd        = 0x060000;
constexpr quint32 kFlashReadBlockSize  = 192;
constexpr quint32 kTransferChunkSize   = 256;

constexpr quint32 kEraseRoutineRamAddr = 0x805568;
constexpr quint32 kWriteRoutineRamAddr = 0x8054AC;

// RAM-resident erase/write helper routines, carried over verbatim (not
// recompiled) from externals/livemonitor/colt_flasher.xml. Valid only for
// ROM 47110032 (Colt CZT, Z37A).
extern const quint8 kErasePageRoutine[160];
extern const quint8 kWritePageRoutine[176];

// sk = (pk * 135 + 1542) mod 65536. Mitsubishi-specific; ported from
// ObdSessionInitCommandSequence::messageCallbackCustomAction in
// externals/livemonitor/obdengine.cpp. Different from (and not compatible
// with) the table-based algorithms used by the Subaru-targeted modules in
// this codebase.
quint16 seedKeyWord(quint16 seedWord);

// Applies seedKeyWord() to both 16-bit halves of a 4-byte seed (big-endian
// in, big-endian out): seed = [pk1_hi, pk1_lo, pk2_hi, pk2_lo].
QByteArray seedKey(const QByteArray &seed);

// 16-bit running sum of every raw byte, with natural wraparound. Matches
// get_crc() in externals/livemonitor/obdengine.cpp — not a real CRC.
quint16 checksum(const QByteArray &data);

// SID 0x34: [SID][start>>16][start>>8][start][0x00][size>>16][size>>8][size].
QByteArray buildRequestDownloadFrame(quint32 start, quint32 size);

// SID 0x36, chunked at kTransferChunkSize bytes per frame: [SID][up to 256 payload bytes].
QVector<QByteArray> buildTransferDataFrames(const QByteArray &payload);

// SID 0x31/225: [SID][225][selector]. selector = 2 if targetStart < 0x800000
// ("flash"), else 1 ("memory") — matches obdengine.cpp's "flash/memory fun".
QByteArray buildRoutineCheckCrcFrame(quint32 targetStart);

// SID 0x31/224, bare 2 bytes, no selector byte. Source comment: "causes reflash".
QByteArray buildRoutineEraseFrame();

// SID 0x3B, 12-byte payload ported verbatim from
// externals/livemonitor/obdsessionwidget.cpp:180-181. KNOWN RISK: the
// original author's comment on this exact payload reads "caused bootloader
// lockup" during their own testing. Treat any call site as the highest-risk
// step in this protocol; never send without an explicit confirmation gate
// on a bench/spare ECU.
QByteArray buildRequestReflashUnlockFrame();

// SID 0x23: [SID][addr>>16][addr>>8][addr][len].
QByteArray buildReadMemoryByAddressFrame(quint32 addr, quint8 len);

// SID 0x10: [SID][sessionId].
QByteArray buildDiagnosticSessionFrame(quint8 sessionId);

// SID 0x27/5 (seed request): [SID][0x05].
QByteArray buildSecurityAccessSeedRequestFrame();

// SID 0x27/6 (key answer): [SID][0x06][4-byte key].
QByteArray buildSecurityAccessKeyFrame(const QByteArray &key);

} // namespace MitsuColtCan
