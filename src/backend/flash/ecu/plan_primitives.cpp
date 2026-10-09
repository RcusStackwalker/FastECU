#include "src/backend/flash/ecu/plan_primitives.h"

namespace fastecu::flash::detail
{
Status validate_regions(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.transfer_region().start != device.fblocks[0].start || plan.transfer_region().length != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, "transfer region does not match the MCU");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.erase_regions().empty()
                   ? Status{}
                   : fail(ErrorKind::InvalidConfig, "read plans must not declare erase regions");
    }
    if (!erase_geometry_matches(plan.erase_regions(), device))
    {
        return fail(ErrorKind::InvalidConfig, "erase geometry does not match the MCU");
    }
    return {};
}

bool erase_geometry_matches(std::span<const MemoryRegion> regions, const FlashDevice& device)
{
    if (regions.size() != device.numblocks)
    {
        return false;
    }
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        if (regions[index].start != device.fblocks[index].start || regions[index].length != device.fblocks[index].len)
        {
            return false;
        }
    }
    return true;
}

std::vector<MemoryRegion> make_erase_regions(const FlashDevice& device)
{
    std::vector<MemoryRegion> regions;
    regions.reserve(device.numblocks);
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        regions.push_back({device.fblocks[index].start, device.fblocks[index].len});
    }
    return regions;
}
} // namespace fastecu::flash::detail
