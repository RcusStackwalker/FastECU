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
int reqLen(int channelCount)
{
    return static_cast<int>(static_cast<unsigned>(channelCount + 3) >> 2U) + channelCount * 2 + 0x1c;
}
bytes::Bytes buildIdListFrame(bytes::Byte listCmd, const std::vector<Channel>& channels)
{
    const int n = static_cast<int>(channels.size());
    const int total = reqLen(n);
    bytes::Bytes f(static_cast<std::size_t>(total), 0);
    f[0] = listCmd;
    f[1] = static_cast<bytes::Byte>(n);
    const int descBytes = static_cast<int>(static_cast<unsigned>(n + 3) >> 2U);
    for (int i = 0; i < n; ++i)
    { // pack 2-bit size descriptors, MSB-first
        const auto ui = static_cast<unsigned>(i);
        const bytes::Byte d = sizeToDescriptor(channels.at(i).len) & 0x3U;
        const unsigned shift = (3U - (ui & 3U)) * 2U;
        const auto descIdx = static_cast<std::size_t>(2) + (ui >> 2U);
        f[descIdx] = static_cast<bytes::Byte>(f[descIdx] | (static_cast<unsigned>(d) << shift));
    }
    int idOff = 2 + descBytes;
    for (int i = 0; i < n; ++i)
    { // big-endian u16 ids
        bytes::writeU16Be(f, static_cast<std::size_t>(idOff), channels.at(i).id);
        idOff += 2;
    }
    f[total - 2] = sum8(f, 0, static_cast<std::size_t>(total - 2));
    f[total - 1] = TRAILER_STD;
    return f;
}
int responseDataLength(const std::vector<Channel>& channels)
{
    int n = 0;
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
    int off = 0;
    for (const Channel& c : channels)
    {
        out.push_back(bytes::readUBe(data, static_cast<std::size_t>(off), c.len));
        off += c.len;
    }
    return out;
}
} // namespace mutdma
