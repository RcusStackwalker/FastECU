#pragma once
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"

#include <QVector>

// TRANSITIONAL Qt shim: QVector<Channel>/QVector<uint32_t> overloads delegating
// to the portable std::vector-based mut_dma free functions, for callers not yet
// converted (src/backend/protocol/mut_dma_driver.{h,cpp}, src/ui/desktop's bench
// memory helpers). C++ allows overloading free functions across headers by
// parameter type, so buildIdListFrame/responseDataLength/decodeStreamValues/
// reassembleRead keep their names here; planReadChannels can't be overloaded on
// return type alone (its parameters are unchanged uint16_t/int), so its Qt-typed
// counterpart is named planReadChannelsQt.
namespace mutdma
{
inline std::vector<Channel> fromQVector(const QVector<Channel>& channels)
{
    return std::vector<Channel>(channels.begin(), channels.end());
}

inline QVector<Channel> toQVector(const std::vector<Channel>& channels)
{
    QVector<Channel> out;
    out.reserve(static_cast<qsizetype>(channels.size()));
    for (const Channel& c : channels)
    {
        out.append(c);
    }
    return out;
}

inline std::vector<std::uint32_t> fromQVector(const QVector<std::uint32_t>& values)
{
    return std::vector<std::uint32_t>(values.begin(), values.end());
}

inline QVector<std::uint32_t> toQVector(const std::vector<std::uint32_t>& values)
{
    QVector<std::uint32_t> out;
    out.reserve(static_cast<qsizetype>(values.size()));
    for (std::uint32_t v : values)
    {
        out.append(v);
    }
    return out;
}

inline bytes::Bytes buildIdListFrame(bytes::Byte listCmd, const QVector<Channel>& channels)
{
    return buildIdListFrame(listCmd, fromQVector(channels));
}

inline int responseDataLength(const QVector<Channel>& channels)
{
    return responseDataLength(fromQVector(channels));
}

inline QVector<std::uint32_t> decodeStreamValues(const QVector<Channel>& channels, bytes::ByteView data)
{
    return toQVector(decodeStreamValues(fromQVector(channels), data));
}

inline QVector<Channel> planReadChannelsQt(std::uint16_t addr, int len)
{
    return toQVector(planReadChannels(addr, len));
}

inline bytes::Bytes reassembleRead(const QVector<std::uint32_t>& values)
{
    return reassembleRead(fromQVector(values));
}
} // namespace mutdma
