#include "src/backend/flash/ecu/plan_primitives.h"

namespace fastecu::flash::detail
{
Status ValidateRegions(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.TransferRegion().start != device.fblocks[0].start || plan.TransferRegion().length != device.romsize)
    {
        return Fail(ErrorKind::kInvalidConfig, "transfer region does not match the MCU");
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        return plan.EraseRegions().empty()
                   ? Status{}
                   : Fail(ErrorKind::kInvalidConfig, "read plans must not declare erase regions");
    }
    if (!EraseGeometryMatches(plan.EraseRegions(), device))
    {
        return Fail(ErrorKind::kInvalidConfig, "erase geometry does not match the MCU");
    }
    return {};
}

bool EraseGeometryMatches(std::span<const MemoryRegion> regions, const FlashDevice& device)
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

std::vector<MemoryRegion> MakeEraseRegions(const FlashDevice& device)
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
