#pragma once
#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/qt_bytes.h"

#include <QByteArray>
#include <QVector>

// TRANSITIONAL Qt shim: QByteArray/QVector overloads delegating to the
// portable bytes::-based Colt protocol functions in mitsu_colt_can_protocol,
// mitsu_colt_can_vendor_ext_protocol, and mitsu_colt_can_cdbg_protocol, for
// callers not yet converted (src/backend/protocol/mitsu_colt_can_cdbg_driver
// and src/backend/flash/ecu/flash_ecu_mitsu_m32r_can_operation). C++ allows
// overloading free functions across headers by parameter type, so
// seedKey/checksum/buildTransferDataFrames/bytesToSeed/
// batchChannelsIntoFrames/buildFrameInitFrames/decodeFrame keep their names
// here; the remaining wrappers (build*Frame) are named distinctly from their
// bytes::-native counterparts already, matching the original single-header
// layout this shim was split out of.
namespace MitsuColtCan
{

inline QByteArray seedKey(const QByteArray& seed)
{
    return bytes::toQByteArray(seedKey(bytes::view(seed)));
}

inline std::uint16_t checksum(const QByteArray& data)
{
    return checksum(bytes::view(data));
}

// SID 0x34: [SID][start>>16][start>>8][start][0x00][size>>16][size>>8][size].
inline QByteArray buildRequestDownloadFrame(std::uint32_t start, std::uint32_t size)
{
    return bytes::toQByteArray(buildRequestDownload(start, size));
}

inline QVector<QByteArray> buildTransferDataFrames(const QByteArray& payload)
{
    QVector<QByteArray> frames;
    const std::vector<bytes::Bytes> byteFrames = buildTransferDataFrames(bytes::view(payload));
    frames.reserve(static_cast<qsizetype>(byteFrames.size()));
    for (const bytes::Bytes& frame : byteFrames)
    {
        frames.append(bytes::toQByteArray(frame));
    }
    return frames;
}

// SID 0x31/225: selector = 2 if targetStart < 0x800000 ("flash"), else 1 ("memory").
inline QByteArray buildRoutineCheckCrcFrame(std::uint32_t targetStart)
{
    return bytes::toQByteArray(buildRoutineCheckCrc(targetStart));
}

// SID 0x31/224, bare 2 bytes, no selector byte.
inline QByteArray buildRoutineEraseFrame()
{
    return bytes::toQByteArray(buildRoutineErase());
}

// SID 0x3B, 12-byte payload. See buildRequestReflashUnlock's KNOWN RISK doc.
inline QByteArray buildRequestReflashUnlockFrame()
{
    return bytes::toQByteArray(buildRequestReflashUnlock());
}

// SID 0x23: [SID][addr>>16][addr>>8][addr][len].
inline QByteArray buildReadMemoryByAddressFrame(std::uint32_t addr, bytes::Byte len)
{
    return bytes::toQByteArray(buildReadMemoryByAddress(addr, len));
}

// SID 0x10: [SID][sessionId].
inline QByteArray buildDiagnosticSessionFrame(bytes::Byte sessionId)
{
    return bytes::toQByteArray(buildDiagnosticSession(sessionId));
}

// SID 0x27/5 (seed request): [SID][0x05].
inline QByteArray buildSecurityAccessSeedRequestFrame()
{
    return bytes::toQByteArray(buildSecurityAccessSeedRequest());
}

// SID 0x27/6 (key answer): [SID][0x06][4-byte key].
inline QByteArray buildSecurityAccessKeyFrame(const QByteArray& key)
{
    return bytes::toQByteArray(buildSecurityAccessKey(bytes::view(key)));
}

} // namespace MitsuColtCan

namespace MitsuColtCanVendorExt
{

// Big-endian byte-array convenience wrappers matching the 4-byte wire
// layout, mirroring MitsuColtCan::seedKey's shape.
inline std::uint32_t bytesToSeed(const QByteArray& seedBytes) // expects exactly 4 bytes
{
    return bytesToSeed(bytes::view(seedBytes));
}

inline QByteArray keyToBytes(std::uint32_t key) // produces exactly 4 bytes
{
    return bytes::toQByteArray(keyBytes(key));
}

inline QByteArray buildChallengeSeedRequestFrame()
{
    return bytes::toQByteArray(buildChallengeSeedRequest());
}

inline QByteArray buildChallengeKeyFrame(std::uint32_t key)
{
    return bytes::toQByteArray(buildChallengeKey(key));
}

} // namespace MitsuColtCanVendorExt

namespace MitsuColtCanCdbg
{

inline std::vector<CdbgChannel> fromQVector(const QVector<CdbgChannel>& channels)
{
    return std::vector<CdbgChannel>(channels.begin(), channels.end());
}

inline QVector<CdbgChannel> toQVector(const std::vector<CdbgChannel>& channels)
{
    QVector<CdbgChannel> out;
    out.reserve(static_cast<qsizetype>(channels.size()));
    for (const CdbgChannel& c : channels)
    {
        out.append(c);
    }
    return out;
}

inline bool batchChannelsIntoFrames(const QVector<CdbgChannel>& channels,
                                    QVector<QVector<CdbgChannel>>& outFrames)
{
    std::vector<std::vector<CdbgChannel>> portableFrames;
    if (!batchChannelsIntoFrames(fromQVector(channels), portableFrames))
    {
        return false;
    }
    QVector<QVector<CdbgChannel>> frames;
    frames.reserve(static_cast<qsizetype>(portableFrames.size()));
    for (const std::vector<CdbgChannel>& frame : portableFrames)
    {
        frames.append(toQVector(frame));
    }
    outFrames = frames;
    return true;
}

inline std::vector<CdbgFrame> buildFrameInitFrames(bytes::Byte instance, bytes::Byte frameIndex,
                                                   const QVector<CdbgChannel>& frameItems)
{
    return buildFrameInitFrames(instance, frameIndex, fromQVector(frameItems));
}

inline QVector<std::uint32_t> decodeFrame(bytes::Byte expectedFrameIndex,
                                          const QVector<CdbgChannel>& frameItems,
                                          bytes::ByteView frame)
{
    const std::vector<std::uint32_t> values = decodeFrame(expectedFrameIndex, fromQVector(frameItems), frame);
    QVector<std::uint32_t> out;
    out.reserve(static_cast<qsizetype>(values.size()));
    for (std::uint32_t v : values)
    {
        out.append(v);
    }
    return out;
}

} // namespace MitsuColtCanCdbg
