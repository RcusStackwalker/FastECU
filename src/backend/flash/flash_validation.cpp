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

bool region_overflows(const MemoryRegion& region)
{
    return static_cast<std::uint64_t>(region.start) + region.length > static_cast<std::uint64_t>(0xffffffffU);
}

// Each alternative's declared family and transport come from FamilyTraits in
// flash_types.h, next to the variant itself, so this stays a single visit
// rather than a switch that has to be kept in step with the variant by hand.
bool family_matches_transport_variant(const FlashPlanFields& fields)
{
    return std::visit(
        [&fields]<typename T>(const T&)
        { return fields.family == FamilyTraits<T>::family && fields.transport == FamilyTraits<T>::transport; },
        fields.family_plan);
}
} // namespace

Result<FlashPlan> validate_and_build(FlashPlanFields fields)
{
    if (fields.target_id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "target_id must not be empty");
    }
    if (fields.mcu_name.empty())
    {
        return fail(ErrorKind::InvalidConfig, "mcu_name must not be empty");
    }
    if (fields.transfer_region.length == 0)
    {
        return fail(ErrorKind::InvalidConfig, "transfer_region must not be empty");
    }
    if (region_overflows(fields.transfer_region))
    {
        return fail(ErrorKind::InvalidConfig, "transfer_region overflows a 32-bit address space");
    }
    for (const MemoryRegion& erase : fields.erase_regions)
    {
        if (region_overflows(erase))
        {
            return fail(ErrorKind::InvalidConfig, "erase region overflows a 32-bit address space");
        }
    }
    if (fields.operation == FlashOperation::Read)
    {
        if (!fields.erase_regions.empty())
        {
            return fail(ErrorKind::InvalidConfig, "Read plans must not declare erase regions");
        }
        if (fields.image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Read plans must not carry an image");
        }
    }
    else
    {
        if (!fields.image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Write/TestWrite plans must carry an image");
        }
    }
    if (const bool requires_kernel =
            std::visit([]<typename T>(const T&) { return family_requires_kernel_v<T>; }, fields.family_plan);
        requires_kernel && !fields.kernel.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "family requires a kernel image");
    }
    if (fields.kernel.has_value())
    {
        if (fields.kernel->id.empty())
        {
            return fail(ErrorKind::InvalidConfig, "kernel id must not be empty");
        }
        if (fields.kernel->bytes.empty())
        {
            return fail(ErrorKind::InvalidConfig, "kernel bytes must not be empty");
        }
        const std::uint64_t kernel_end =
            static_cast<std::uint64_t>(fields.kernel->load_address) + fields.kernel->bytes.size();
        if (kernel_end > static_cast<std::uint64_t>(0xffffffffU))
        {
            return fail(ErrorKind::InvalidConfig, "kernel upload range overflows a 32-bit address space");
        }
    }
    if (!family_matches_transport_variant(fields))
    {
        return fail(ErrorKind::InvalidConfig, "family_plan variant does not match transport kind or declared family");
    }
    std::unordered_set<ConfirmationSpec::Id> seen_ids;
    for (const ConfirmationSpec& confirmation : fields.confirmations)
    {
        if (!seen_ids.insert(confirmation.id).second)
        {
            return fail(ErrorKind::InvalidConfig, "duplicate confirmation id declared");
        }
    }

    const std::uint64_t total_transfer_bytes = fields.transfer_region.length;
    return FlashPlan(std::move(fields), total_transfer_bytes);
}

} // namespace fastecu::flash
