// src/backend/flash/transfer_progress.cpp
#include "src/backend/flash/transfer_progress.h"

#include <algorithm>
#include <format>
#include <limits>

namespace fastecu::flash
{

TransferRate ComputeTransferRate(std::uint64_t bytes, std::uint64_t elapsed_ms, std::uint64_t remaining_bytes)
{
    const std::uint64_t speed = std::clamp<std::uint64_t>(bytes * 1000U / std::max<std::uint64_t>(elapsed_ms, 1U), 1U,
                                                          std::numeric_limits<std::uint32_t>::max());
    const std::uint64_t eta = std::min<std::uint64_t>(remaining_bytes / speed + 1U, kMaxEtaSeconds);
    return TransferRate{.speed = static_cast<std::uint32_t>(speed), .eta_s = static_cast<std::uint32_t>(eta)};
}

std::string FormatReadProgress(std::uint32_t address, std::uint32_t length, TransferRate rate)
{
    return std::format("Kernel read addr: 0x{:08X} length: 0x{:08X}, {:>6} B/s {:>6} s", address, length, rate.speed,
                       rate.eta_s);
}

std::string FormatWriteProgress(std::uint32_t address, std::uint32_t percent, TransferRate rate)
{
    return std::format("Write flash buffer: 0x{:08X} ({}% - {} B/s, ~ {} s)", address, percent, rate.speed, rate.eta_s);
}

} // namespace fastecu::flash
