#include "src/backend/flash/ecu/plan_primitives.h"

namespace fastecu::flash::detail
{
bool erase_geometry_matches(std::span<const MemoryRegion> regions, const flashdev_t& device)
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

std::vector<MemoryRegion> make_erase_regions(const flashdev_t& device)
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
