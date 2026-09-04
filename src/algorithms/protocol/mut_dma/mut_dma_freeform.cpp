#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"

#include <cstddef>

namespace mutdma
{
MutDmaFrame buildSetupFrame(bytes::Byte setupCmd, bytes::Byte channelCount)
{
    const bytes::Bytes payload{channelCount};
    return buildCommandFrame(setupCmd, payload, TRAILER_FREEFORM);
}
bytes::Byte sizeToDescriptor(bytes::Byte len)
{
    switch (len)
    {
    case 1:
        return 0;
    case 2:
        return 1;
    case 4:
        return 2;
    default:
        return 0;
    }
}
std::size_t reqLen(std::size_t channelCount)
{
    return (channelCount + 3) / 4 + channelCount * 2 + 0x1c;
}
bytes::Bytes buildIdListFrame(bytes::Byte listCmd, const std::vector<Channel>& channels)
{
    const std::size_t n = channels.size();
    const std::size_t total = reqLen(n);
    bytes::Bytes f(total, 0);
    f[0] = listCmd;
    f[1] = static_cast<bytes::Byte>(n);
    const std::size_t descBytes = (n + 3) / 4;
    for (std::size_t i = 0; i < n; ++i)
    { // pack 2-bit size descriptors, MSB-first
        const bytes::Byte d = sizeToDescriptor(channels[i].len) & 0x3U;
        const std::size_t shift = (3 - i % 4) * 2;
        const std::size_t descIdx = 2 + i / 4;
        f[descIdx] = static_cast<bytes::Byte>(f[descIdx] | (static_cast<unsigned>(d) << shift));
    }
    std::size_t idOff = 2 + descBytes;
    for (const Channel& channel : channels)
    { // big-endian u16 ids
        bytes::writeU16Be(f, idOff, channel.id);
        idOff += 2;
    }
    f[total - 2] = sum8(f, 0, total - 2);
    f[total - 1] = TRAILER_STD;
    return f;
}
std::size_t responseDataLength(const std::vector<Channel>& channels)
{
    std::size_t n = 0;
    for (const Channel& c : channels)
    {
        n += c.len;
    }
    return n;
}
std::vector<std::uint32_t> decodeStreamValues(const std::vector<Channel>& channels, bytes::ByteView data)
{
    std::vector<std::uint32_t> out;
    out.reserve(channels.size());
    std::size_t off = 0;
    for (const Channel& c : channels)
    {
        out.push_back(bytes::readUBe(data, off, c.len));
        off += c.len;
    }
    return out;
}
} // namespace mutdma
