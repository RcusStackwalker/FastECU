#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"

#include <algorithm>

namespace mutdma
{

bytes::Byte sum8(bytes::ByteView bytes, std::size_t from, std::size_t len)
{
    return bytes::sum8Range(bytes, from, len);
}

bytes::Byte sum8(bytes::ByteView bytes)
{
    return bytes::sum8(bytes);
}

MutDmaFrame buildCommandFrame(bytes::Byte cmd, bytes::ByteView payload, bytes::Byte trailer)
{
    MutDmaFrame f{};
    f[0] = cmd;
    const std::size_t n =
        std::min(payload.size(), static_cast<std::size_t>(kChecksumOffset - 1)); // bytes 1..48 = 48 max
    std::copy_n(payload.begin(), n, f.begin() + 1);
    f[kChecksumOffset] = sum8(bytes::ByteView{f}.first(kChecksumOffset));
    f[kTrailerOffset] = trailer;
    return f;
}

bool verifyFrame(bytes::ByteView frame)
{
    if (frame.size() != kFrameLen)
    {
        return false;
    }
    if (frame[kChecksumOffset] != sum8(frame, 0, kChecksumOffset))
    {
        return false;
    }
    const bytes::Byte t = frame[kTrailerOffset];
    return t == kTrailerStd || t == kTrailerFreeform;
}

StreamFrame parseStreamFrame(bytes::ByteView frame)
{
    StreamFrame s;
    if (frame.size() < 3)
    {
        return s; // id + csum + trailer minimum
    }
    if (frame[frame.size() - 1] != kTrailerStd)
    {
        return s;
    }
    const std::size_t csum_idx = frame.size() - 2;
    if (frame[csum_idx] != sum8(frame, 0, csum_idx))
    {
        return s;
    }
    s.log_id = frame[0];
    s.data.assign(frame.begin() + 1, frame.begin() + static_cast<std::ptrdiff_t>(csum_idx));
    s.ok = true;
    return s;
}
} // namespace mutdma
