#include "protocol/mut_dma_freeform.h"

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
    return ((channelCount + 3) >> 2) + channelCount * 2 + 0x1c;
}
bytes::Bytes buildIdListFrame(bytes::Byte listCmd, const QVector<Channel>& channels)
{
    const int n = channels.size();
    const int total = reqLen(n);
    bytes::Bytes f(static_cast<std::size_t>(total), 0);
    f[0] = listCmd;
    f[1] = static_cast<bytes::Byte>(n);
    const int descBytes = (n + 3) >> 2;
    for (int i = 0; i < n; ++i)
    { // pack 2-bit size descriptors, MSB-first
        const bytes::Byte d = sizeToDescriptor(channels.at(i).len) & 0x3;
        const int shift = (3 - (i & 3)) * 2;
        f[2 + (i >> 2)] = static_cast<bytes::Byte>(f[2 + (i >> 2)] | (d << shift));
    }
    int idOff = 2 + descBytes;
    for (int i = 0; i < n; ++i)
    { // big-endian u16 ids
        f[idOff++] = static_cast<bytes::Byte>(channels.at(i).id >> 8);
        f[idOff++] = static_cast<bytes::Byte>(channels.at(i).id & 0xFF);
    }
    f[total - 2] = sum8(f, 0, static_cast<std::size_t>(total - 2));
    f[total - 1] = TRAILER_STD;
    return f;
}
int responseDataLength(const QVector<Channel>& channels)
{
    int n = 0;
    for (const Channel& c : channels)
        n += c.len;
    return n;
}
QVector<std::uint32_t> decodeStreamValues(const QVector<Channel>& channels, bytes::ByteView data)
{
    QVector<std::uint32_t> out;
    int off = 0;
    for (const Channel& c : channels)
    {
        std::uint32_t v = 0;
        for (int k = 0; k < c.len && off < static_cast<int>(data.size()); ++k, ++off)
            v = (v << 8) | data[static_cast<std::size_t>(off)]; // big-endian
        out.append(v);
    }
    return out;
}
} // namespace mutdma
