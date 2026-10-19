#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>

namespace mutdma
{
std::vector<MutDmaFrame> BuildWriteFrames(std::uint16_t addr, bytes::ByteView bytes)
{
    if (std::uint32_t(addr) + std::uint32_t(bytes.size()) > 0x10000U)
    {
        return {};
    }
    std::vector<MutDmaFrame> frames;
    std::size_t off = 0;
    while (off < bytes.size())
    {
        const std::size_t chunk = std::min(static_cast<std::size_t>(kMaxWriteChunk), bytes.size() - off);
        const std::uint16_t a = static_cast<std::uint16_t>(addr + off);
        bytes::Bytes payload;
        payload.reserve(5 + chunk);
        payload.push_back(0x00); // sub-selector hi
        payload.push_back(0x03); // sub-selector lo = write arbitrary
        bytes::AppendU16Be(payload, a);
        payload.push_back(static_cast<bytes::Byte>(chunk));
        payload.insert(payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(off),
                       bytes.begin() + static_cast<std::ptrdiff_t>(off + chunk));
        frames.push_back(BuildCommandFrame(0x87, payload, kTrailerStd));
        off += chunk;
    }
    return frames;
}
std::vector<Channel> PlanReadChannels(std::uint16_t addr, int len)
{
    return std::views::iota(0, len) |
           std::views::transform([addr](int i) { return Channel{static_cast<std::uint16_t>(addr + i), 1}; }) |
           std::ranges::to<std::vector<Channel>>();
}
bytes::Bytes ReassembleRead(const std::vector<std::uint32_t>& values)
{
    return values | std::views::transform([](std::uint32_t v) { return static_cast<bytes::Byte>(v & 0xFF); }) |
           std::ranges::to<bytes::Bytes>();
}
} // namespace mutdma
