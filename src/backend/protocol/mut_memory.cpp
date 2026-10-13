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
constexpr std::size_t kWritableEnd = 0xC000;
constexpr std::size_t kAddressSpaceEnd = 0x10000;
constexpr int kMutBaud = 125000;
constexpr std::size_t kReadChunk = 40;
constexpr bytes::Byte kSetupCmd = 0xA0;
constexpr bytes::Byte kListCmd = 0xA1;
constexpr std::chrono::milliseconds kPollTimeout{50};
} // namespace

fastecu::Status WriteMemory(IKlineTransport& transport, std::uint16_t addr, bytes::ByteView data,
                            const fastecu::ICancellationToken& cancellation)
{
    if (addr < kWritableLow || addr >= kWritableEnd || data.size() > kWritableEnd - addr)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "MUT/DMA: refusing write outside 0x4000-0xBFFF");
    }
    AlreadyInMode init(kMutBaud);
    MutDmaDriver driver(transport, init);
    return driver.WriteMemory(addr, data, cancellation);
}

fastecu::Result<bytes::Bytes> ReadMemory(IKlineTransport& transport, std::uint16_t addr, std::size_t len,
                                         const fastecu::ICancellationToken& cancellation)
{
    if (len > kAddressSpaceEnd - addr)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "MUT/DMA: refusing read beyond 16-bit address space");
    }
    AlreadyInMode init(kMutBaud);
    MutDmaDriver driver(transport, init);
    bytes::Bytes out;
    for (std::size_t off = 0; off < len; off += kReadChunk)
    {
        const std::size_t chunk = std::min(kReadChunk, len - off);
        const std::vector<Channel> channels =
            PlanReadChannels(static_cast<std::uint16_t>(addr + off), static_cast<int>(chunk));
        if (auto started = driver.StartFreeFormLog(channels, kSetupCmd, kListCmd, cancellation); !started.has_value())
        {
            return std::unexpected(started.error());
        }
        auto values = driver.PollOnce(kPollTimeout, cancellation);
        if (!values.has_value())
        {
            return std::unexpected(values.error());
        }
        if (values->empty())
        {
            return fastecu::Fail(fastecu::ErrorKind::kTimeout, "MUT/DMA: no frame for memory read chunk");
        }
        const bytes::Bytes piece = ReassembleRead(*values);
        if (piece.size() != chunk)
        {
            return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "MUT/DMA: memory read chunk has unexpected length");
        }
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

} // namespace mutdma
