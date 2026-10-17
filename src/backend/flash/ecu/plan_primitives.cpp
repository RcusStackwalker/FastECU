#include "src/backend/flash/ecu/plan_primitives.h"

namespace fastecu::flash::detail
{
bool FixedShapeDeviceMatches(const FlashDevice *device, std::uint32_t rom_size, unsigned block_count)
{
    return device != nullptr && device->fblocks != nullptr && device->kblocks != nullptr &&
           device->romsize == rom_size && device->numblocks == block_count;
}

bool OperationImageShapeMatches(const FlashPlan& plan, std::uint32_t rom_size)
{
    return plan.Operation() == FlashOperation::kRead ? !plan.Image().has_value()
                                                     : plan.Image().has_value() && plan.Image()->size() == rom_size;
}

bool TransferMatchesDevice(const MemoryRegion& transfer, const FlashDevice& device)
{
    return transfer.start == device.fblocks[0].start && transfer.length == device.romsize;
}

bool RequiresOnly(std::span<const ConfirmationSpec> confirmations, ConfirmationSpec::Id id)
{
    return confirmations.size() == 1 && confirmations.front() == ConfirmationSpec{.id = id};
}

Status ValidateRegions(const FlashPlan& plan, const FlashDevice& device)
{
    if (!TransferMatchesDevice(plan.TransferRegion(), device))
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
