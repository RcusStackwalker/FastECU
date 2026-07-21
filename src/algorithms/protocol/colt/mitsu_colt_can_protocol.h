#pragma once
#include "src/algorithms/protocol/bytes.h"

#include <cstdint>
#include <vector>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash protocol.
// Ported from externals/livemonitor/obdengine.cpp + colt_flasher.xml.
// Request CAN ID 0x7E0, reply CAN ID 0x7E8 (ISO 15765 / UDS-style OBD).
// Pure, hardware-independent frame builders only — no I/O here.
namespace MitsuColtCan
{

constexpr bytes::Byte kServiceDiagnosticSession = 0x10;
constexpr bytes::Byte kServiceSecurityAccess = 0x27;
constexpr bytes::Byte kServiceReadMemoryByAddress = 0x23;
constexpr bytes::Byte kServiceRequestDownload = 0x34;
constexpr bytes::Byte kServiceTransferData = 0x36;
constexpr bytes::Byte kServiceRoutineControl = 0x31;
constexpr bytes::Byte kServiceRequestReflash = 0x3B;

constexpr bytes::Byte kSessionBasic = 0x81;
constexpr bytes::Byte kSessionBootload = 0x85;

constexpr std::uint32_t kCrcTransferAddress = 0x200000;
constexpr std::uint32_t kCrcTransferSize = 2;
constexpr bytes::Byte kRoutineCheckCrc = 225;
constexpr bytes::Byte kRoutineErase = 224;

constexpr std::uint32_t kUserspaceStart = 0x008000;
constexpr std::uint32_t kUserspaceEnd = 0x060000;
constexpr std::uint32_t kFlashReadBlockSize = 192;
constexpr std::uint32_t kTransferChunkSize = 256;

constexpr std::uint32_t kTopRegionStart = 0x060000;
constexpr std::uint32_t kTopRegionEnd = 0x080000;
constexpr std::uint32_t kTopRegionLength = kTopRegionEnd - kTopRegionStart;

constexpr std::uint32_t kEraseRoutineRamAddr = 0x805568;
constexpr std::uint32_t kWriteRoutineRamAddr = 0x8054AC;

// RAM-resident erase/write helper routines, carried over verbatim (not
// recompiled) from externals/livemonitor/colt_flasher.xml. Valid only for
// ROM 47110032 (Colt CZT, Z37A).
extern const bytes::Byte kErasePageRoutine[160];
extern const bytes::Byte kWritePageRoutine[176];

// RAM-resident redirect erase/write helpers. Structural siblings of
// kErasePageRoutine/kWritePageRoutine above (same RAM slots, same command
// sequences), each with a fixed +0x058000 (kTopRegionStart-kUserspaceStart)
// added to the target address before touching flash -- baked into the
// assembled bytes below, not recomputed at runtime. The bootloader's
// RequestDownload handler hard-validates write targets to
// [kUserspaceStart, kUserspaceEnd) (see colt_commented.S ~0x5d4c), and its
// erase-trigger (RoutineControl 224) always sweeps that entire hardcoded
// range regardless of any declared RequestDownload window (~0x59b4), so a
// client-declared carrier address in
// [kUserspaceStart, kUserspaceStart+kTopRegionLength) is what actually
// reaches the otherwise-unwritable kTopRegionStart-kTopRegionEnd range.
// See docs/superpowers/specs/2026-07-11-z37a-top128k-can-reflash-design.md
// in the parent claude-hobby project. erase_redirect32170.S additionally
// bounds-checks the incoming address (only redirects addresses below
// kUserspaceStart+kTopRegionLength; everything else no-ops) since the
// erase-trigger's hardcoded sweep would otherwise walk past the physical
// 512KB flash boundary. Built from
// mmc-patches/m32r/47110032/reflash/{erase,write}_redirect32170.S.
extern const bytes::Byte kEraseRedirectRoutine[192];
extern const bytes::Byte kWriteRedirectRoutine[188];

// sk = (pk * 135 + 1542) mod 65536. Mitsubishi-specific; ported from
// ObdSessionInitCommandSequence::messageCallbackCustomAction in
// externals/livemonitor/obdengine.cpp. Different from (and not compatible
// with) the table-based algorithms used by the Subaru-targeted modules in
// this codebase.
std::uint16_t seedKeyWord(std::uint16_t seedWord);

// Applies seedKeyWord() to both 16-bit halves of a 4-byte seed (big-endian
// in, big-endian out): seed = [pk1_hi, pk1_lo, pk2_hi, pk2_lo].
bytes::Bytes seedKey(bytes::ByteView seed);

// 16-bit running sum of every raw byte, with natural wraparound. Matches
// get_crc() in externals/livemonitor/obdengine.cpp — not a real CRC.
std::uint16_t checksum(bytes::ByteView data);

// SID 0x34: [SID][start>>16][start>>8][start][0x00][size>>16][size>>8][size].
bytes::Bytes buildRequestDownload(std::uint32_t start, std::uint32_t size);

// SID 0x36, chunked at kTransferChunkSize bytes per frame: [SID][up to 256 payload bytes].
std::vector<bytes::Bytes> buildTransferDataFrames(bytes::ByteView payload);

// SID 0x31/225: [SID][225][selector]. selector = 2 if targetStart < 0x800000
// ("flash"), else 1 ("memory") — matches obdengine.cpp's "flash/memory fun".
bytes::Bytes buildRoutineCheckCrc(std::uint32_t targetStart);

// SID 0x31/224, bare 2 bytes, no selector byte. Source comment: "causes reflash".
bytes::Bytes buildRoutineErase();

// SID 0x3B, 12-byte payload ported verbatim from
// externals/livemonitor/obdsessionwidget.cpp:180-181. KNOWN RISK: the
// original author's comment on this exact payload reads "caused bootloader
// lockup" during their own testing. Treat any call site as the highest-risk
// step in this protocol; never send without an explicit confirmation gate
// on a bench/spare ECU.
bytes::Bytes buildRequestReflashUnlock();

// SID 0x23: [SID][addr>>16][addr>>8][addr][len].
bytes::Bytes buildReadMemoryByAddress(std::uint32_t addr, bytes::Byte len);

// SID 0x10: [SID][sessionId].
bytes::Bytes buildDiagnosticSession(bytes::Byte sessionId);

// SID 0x27/5 (seed request): [SID][0x05].
bytes::Bytes buildSecurityAccessSeedRequest();

// SID 0x27/6 (key answer): [SID][0x06][4-byte key].
bytes::Bytes buildSecurityAccessKey(bytes::ByteView key);

} // namespace MitsuColtCan
