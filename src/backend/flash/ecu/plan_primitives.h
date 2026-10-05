#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "src/backend/flash/flash_types.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash::detail
{
// Keep the caller's padding arithmetic width: the diesel family historically
// uses size_t, while petrol, TCU and DensoCAN use uint64_t. Address arithmetic
// is widened independently so a high MCU address cannot wrap or underflow.
// Identity/address selection and empty-kernel validation stay in the family.
template <std::unsigned_integral Size, Size BlockSize>
Status validate_padded_kernel_range(Size size, std::uint32_t load_address, const kernelblock& region,
                                    const char *padding_error, const char *range_error)
{
    static_assert(BlockSize > 0);
    constexpr Size kPadding = BlockSize - 1;
    if (size > std::numeric_limits<Size>::max() - kPadding)
    {
        return fail(ErrorKind::InvalidConfig, padding_error);
    }
    const Size padded_size = ((size + kPadding) / BlockSize) * BlockSize;
    const std::uint64_t region_start = region.start;
    const std::uint64_t region_end = region_start + region.len;
    const std::uint64_t upload_start = load_address;
    if (upload_start < region_start || upload_start > region_end || padded_size > region_end - upload_start)
    {
        return fail(ErrorKind::InvalidConfig, range_error);
    }
    return {};
}

// Callers validate the device table and retain their family-specific region
// and block-count diagnostics before comparing individual blocks.
bool erase_geometry_matches(std::span<const MemoryRegion> regions, const flashdev_t& device);
std::vector<MemoryRegion> make_erase_regions(const flashdev_t& device);
} // namespace fastecu::flash::detail
