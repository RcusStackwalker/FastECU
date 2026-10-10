#include "src/backend/flash/flash_validation.h"

#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fastecu::flash
{
namespace
{

// True when the region's end address (start + length, one past its last byte)
// cannot be held in a uint32_t.
//
// This is deliberately one stricter than "the region fits in the address
// space": a region whose last byte is 0xffffffff ends at 0x100000000 and is
// rejected here even though every byte of it is addressable. Executors
// compute that end address in uint32_t arithmetic and bound real erase and
// write loops with it -- ecu/mitsu_colt_m32r_can_executor.cpp derives
// writable_end and page_write_end that way, and ecu/mitsu_colt_m32r_can_plan.cpp
// sizes the image against it -- where 0x100000000 would wrap to 0 and collapse
// the window. Accepting it here would relax an address-window guard. The
// boundary is pinned by TransferRegionEndingExactlyAtTheTopOfTheAddressSpaceIsRejected.
bool RegionEndNotRepresentable(const MemoryRegion& region)
{
    return static_cast<std::uint64_t>(region.start) + region.length > static_cast<std::uint64_t>(0xffffffffU);
}

// Each alternative's declared family and transport come from FamilyTraits in
// flash_types.h, next to the variant itself, so this stays a single visit
// rather than a switch that has to be kept in step with the variant by hand.
bool FamilyMatchesTransportVariant(const FlashPlanFields& fields)
{
    return std::visit(
        [&fields]<typename T>(const T&)
        { return fields.family == FamilyTraits<T>::kFamily && fields.transport == FamilyTraits<T>::kTransport; },
        fields.family_plan);
}
} // namespace

Result<FlashPlan> ValidateAndBuild(FlashPlanFields fields)
{
    if (fields.target_id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "target_id must not be empty");
    }
    if (fields.mcu_name.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "mcu_name must not be empty");
    }
    if (fields.transfer_region.length == 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "transfer_region must not be empty");
    }
    if (RegionEndNotRepresentable(fields.transfer_region))
    {
        return Fail(ErrorKind::kInvalidConfig, "transfer_region end address does not fit in 32 bits");
    }
    for (const MemoryRegion& erase : fields.erase_regions)
    {
        if (RegionEndNotRepresentable(erase))
        {
            return Fail(ErrorKind::kInvalidConfig, "erase region end address does not fit in 32 bits");
        }
    }
    if (fields.operation == FlashOperation::kRead)
    {
        if (!fields.erase_regions.empty())
        {
            return Fail(ErrorKind::kInvalidConfig, "Read plans must not declare erase regions");
        }
        if (fields.image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "Read plans must not carry an image");
        }
    }
    else
    {
        if (!fields.image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "Write/TestWrite plans must carry an image");
        }
    }
    if (fields.image_map.has_value() &&
        (!fields.image.has_value() || fields.image_map->FileSize().Value() != fields.image->size()))
    {
        return Fail(ErrorKind::kInvalidConfig, "the image's memory map places a ROM file of another size");
    }
    if (const bool requires_kernel =
            std::visit([]<typename T>(const T&) { return kFamilyRequiresKernel<T>; }, fields.family_plan);
        requires_kernel && !fields.kernel.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "family requires a kernel image");
    }
    if (fields.kernel.has_value())
    {
        if (fields.kernel->id.empty())
        {
            return Fail(ErrorKind::kInvalidConfig, "kernel id must not be empty");
        }
        if (fields.kernel->bytes.empty())
        {
            return Fail(ErrorKind::kInvalidConfig, "kernel bytes must not be empty");
        }
        const std::uint64_t kernel_end =
            static_cast<std::uint64_t>(fields.kernel->load_address) + fields.kernel->bytes.size();
        if (kernel_end > static_cast<std::uint64_t>(0xffffffffU))
        {
            return Fail(ErrorKind::kInvalidConfig, "kernel upload range overflows a 32-bit address space");
        }
    }
    if (!FamilyMatchesTransportVariant(fields))
    {
        return Fail(ErrorKind::kInvalidConfig, "family_plan variant does not match transport kind or declared family");
    }
    std::unordered_set<ConfirmationSpec::Id> seen_ids;
    for (const ConfirmationSpec& confirmation : fields.confirmations)
    {
        if (!seen_ids.insert(confirmation.id).second)
        {
            return Fail(ErrorKind::kInvalidConfig, "duplicate confirmation id declared");
        }
    }

    const std::uint64_t total_transfer_bytes = fields.transfer_region.length;
    return FlashPlan(std::move(fields), total_transfer_bytes);
}

} // namespace fastecu::flash
