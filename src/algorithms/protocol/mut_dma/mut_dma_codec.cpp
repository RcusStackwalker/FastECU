#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"

#include <algorithm>

namespace mutdma
{

bytes::Byte sum8(bytes::ByteView bytes, std::size_t from, std::size_t len)
{
    return bytes::sum8(bytes, from, len);
}

bytes::Byte sum8(bytes::ByteView bytes)
{
    return bytes::sum8(bytes);
}

MutDmaFrame buildCommandFrame(bytes::Byte cmd, bytes::ByteView payload, bytes::Byte trailer)
{
    MutDmaFrame f{};
    f[0] = cmd;
    const std::size_t n = std::min(payload.size(), static_cast<std::size_t>(CHECKSUM_OFFSET - 1)); // bytes 1..48 = 48 max
    std::copy_n(payload.begin(), n, f.begin() + 1);
    f[CHECKSUM_OFFSET] = sum8(bytes::ByteView(f.data(), CHECKSUM_OFFSET));
    f[TRAILER_OFFSET] = trailer;
    return f;
}

bool verifyFrame(bytes::ByteView frame)
{
    if (frame.size() != FRAME_LEN)
    {
        return false;
    }
    if (frame[CHECKSUM_OFFSET] != sum8(frame, 0, CHECKSUM_OFFSET))
    {
        return false;
    }
    const bytes::Byte t = frame[TRAILER_OFFSET];
    return t == TRAILER_STD || t == TRAILER_FREEFORM;
}

StreamFrame parseStreamFrame(bytes::ByteView frame)
{
    StreamFrame s;
    if (frame.size() < 3)
    {
        return s; // id + csum + trailer minimum
    }
    if (frame[frame.size() - 1] != TRAILER_STD)
    {
        return s;
    }
    const std::size_t csumIdx = frame.size() - 2;
    if (frame[csumIdx] != sum8(frame, 0, csumIdx))
    {
        return s;
    }
    s.logId = frame[0];
    s.data.assign(frame.begin() + 1, frame.begin() + static_cast<std::ptrdiff_t>(csumIdx));
    s.ok = true;
    return s;
}
} // namespace mutdma
