#include "src/backend/protocol/mut_memory.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/backend/protocol/mut_dma_driver.h"

namespace mutdma
{
namespace
{
constexpr std::uint16_t kWritableLow = 0x4000;
constexpr std::uint16_t kWritableHigh = 0xBFFF;
constexpr int kMutBaud = 125000;
constexpr std::size_t kReadChunk = 40;
constexpr bytes::Byte kSetupCmd = 0xA0;
constexpr bytes::Byte kListCmd = 0xA1;
constexpr std::chrono::milliseconds kPollTimeout{50};
} // namespace

fastecu::Status write_memory(IKlineTransport& transport, std::uint16_t addr, bytes::ByteView data,
                             const fastecu::ICancellationToken& cancellation)
{
    if (addr < kWritableLow || addr > kWritableHigh)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "MUT/DMA: refusing write outside 0x4000-0xBFFF");
    }
    AlreadyInMode init(kMutBaud);
    MutDmaDriver driver(transport, init);
    return driver.writeMemory(addr, data, cancellation);
}

fastecu::Result<bytes::Bytes> read_memory(IKlineTransport& transport, std::uint16_t addr, std::size_t len,
                                          const fastecu::ICancellationToken& cancellation)
{
    AlreadyInMode init(kMutBaud);
    MutDmaDriver driver(transport, init);
    bytes::Bytes out;
    for (std::size_t off = 0; off < len; off += kReadChunk)
    {
        const std::size_t chunk = std::min(kReadChunk, len - off);
        const std::vector<Channel> channels =
            planReadChannels(static_cast<std::uint16_t>(addr + off), static_cast<int>(chunk));
        if (auto started = driver.startFreeFormLog(channels, kSetupCmd, kListCmd, cancellation); !started.has_value())
        {
            if (off == 0)
            {
                return std::unexpected(started.error());
            }
            break;
        }
        auto values = driver.pollOnce(kPollTimeout, cancellation);
        if (!values.has_value())
        {
            if (off == 0)
            {
                return std::unexpected(values.error());
            }
            break;
        }
        const bytes::Bytes piece = reassembleRead(*values);
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

} // namespace mutdma
