#pragma once
#include <cstdint>
#include <string>

namespace fastecu::flash
{

// Longest estimate ever reported, in seconds.
inline constexpr std::uint32_t kMaxEtaSeconds = 9999;

struct TransferRate
{
    std::uint32_t speed = 1; // bytes per second, never 0
    std::uint32_t eta_s = 1; // seconds remaining, in [1, kMaxEtaSeconds]
};

// Rate of one page: `bytes` moved in `elapsed_ms`, with `remaining_bytes` still
// to go. A zero elapsed time counts as 1 ms; a zero rate is raised to 1 B/s.
TransferRate ComputeTransferRate(std::uint64_t bytes, std::uint64_t elapsed_ms, std::uint64_t remaining_bytes);

// "Kernel read addr: 0x... length: 0x..., N B/s N s"
std::string FormatReadProgress(std::uint32_t address, std::uint32_t length, TransferRate rate);

// "Write flash buffer: 0x... (N% - N B/s, ~ N s)"
std::string FormatWriteProgress(std::uint32_t address, std::uint32_t percent, TransferRate rate);

} // namespace fastecu::flash
