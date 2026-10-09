#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"

#include <cstddef>

namespace mutdma
{
MutDmaFrame BuildSetupFrame(bytes::Byte setup_cmd, bytes::Byte channel_count)
{
    const bytes::Bytes payload{channel_count};
    return BuildCommandFrame(setup_cmd, payload, kTrailerFreeform);
}
bytes::Byte SizeToDescriptor(bytes::Byte len)
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
std::size_t ReqLen(std::size_t channel_count)
{
    return (channel_count + 3) / 4 + channel_count * 2 + 0x1c;
}
bytes::Bytes BuildIdListFrame(bytes::Byte list_cmd, const std::vector<Channel>& channels)
{
    const std::size_t n = channels.size();
    const std::size_t total = ReqLen(n);
    bytes::Bytes f(total, 0);
    f[0] = list_cmd;
    f[1] = static_cast<bytes::Byte>(n);
    const std::size_t desc_bytes = (n + 3) / 4;
    for (std::size_t i = 0; i < n; ++i)
    { // pack 2-bit size descriptors, MSB-first
        const bytes::Byte d = SizeToDescriptor(channels[i].len) & 0x3U;
        const std::size_t shift = (3 - i % 4) * 2;
        const std::size_t desc_idx = 2 + i / 4;
        f[desc_idx] = static_cast<bytes::Byte>(f[desc_idx] | (static_cast<unsigned>(d) << shift));
    }
    std::size_t id_off = 2 + desc_bytes;
    for (const Channel& channel : channels)
    { // big-endian u16 ids
        bytes::WriteU16Be(f, id_off, channel.id);
        id_off += 2;
    }
    f[total - 2] = Sum8(f, 0, total - 2);
    f[total - 1] = kTrailerStd;
    return f;
}
std::size_t ResponseDataLength(const std::vector<Channel>& channels)
{
    std::size_t n = 0;
    for (const Channel& c : channels)
    {
        n += c.len;
    }
    return n;
}
std::vector<std::uint32_t> DecodeStreamValues(const std::vector<Channel>& channels, bytes::ByteView data)
{
    if (data.size() != ResponseDataLength(channels))
    {
        return {};
    }
    std::vector<std::uint32_t> out;
    out.reserve(channels.size());
    std::size_t off = 0;
    for (const Channel& c : channels)
    {
        out.push_back(bytes::ReadUBe(data, off, c.len));
        off += c.len;
    }
    return out;
}
} // namespace mutdma
