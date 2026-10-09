#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash::detail
{
// Expected addresses and block sizes come from each family's protocol.
// Use a numeric size so overflow boundaries can be checked without allocating
// an image. Address and padding arithmetic use 64 bits to avoid wrapping.
// Callers validate the device table and reject empty kernels.
template <std::uint64_t BlockSize>
Status validate_kernel_upload(std::uint64_t size, std::uint32_t load_address, std::uint32_t expected_load_address,
                              const KernelBlock& region)
{
    static_assert(BlockSize > 0);
    if (load_address != expected_load_address)
    {
        return fail(ErrorKind::InvalidConfig, "kernel address does not match the selected protocol");
    }
    constexpr std::uint64_t kPadding = BlockSize - 1;
    if (size > std::numeric_limits<std::uint64_t>::max() - kPadding)
    {
        return fail(ErrorKind::InvalidConfig, "kernel size cannot be padded to transfer blocks");
    }
    const std::uint64_t padded_size = ((size + kPadding) / BlockSize) * BlockSize;
    const std::uint64_t region_start = region.start;
    const std::uint64_t region_end = region_start + region.len;
    const std::uint64_t upload_start = load_address;
    if (upload_start < region_start || upload_start > region_end || padded_size > region_end - upload_start)
    {
        return fail(ErrorKind::InvalidConfig, "padded kernel lies outside the MCU kernel region");
    }
    return {};
}

// Callers validate the device table before comparing the block count and
// ordered addresses and lengths.
Status validate_regions(const FlashPlan& plan, const FlashDevice& device);
bool erase_geometry_matches(std::span<const MemoryRegion> regions, const FlashDevice& device);
std::vector<MemoryRegion> make_erase_regions(const FlashDevice& device);
} // namespace fastecu::flash::detail
