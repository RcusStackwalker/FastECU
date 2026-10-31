#include "src/backend/flash/flash_validation.h"

#include "src/algorithms/memory/memory_image.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/error.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
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

Status ValidateTargetAndRegions(const FlashPlanFields& fields)
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
    if (std::ranges::any_of(fields.erase_regions, RegionEndNotRepresentable))
    {
        return Fail(ErrorKind::kInvalidConfig, "erase region end address does not fit in 32 bits");
    }
    return {};
}

// Reads erase nothing and carry no image; writes and test writes carry one.
Status ValidateOperationShape(const FlashPlanFields& fields)
{
    if (fields.operation != FlashOperation::kRead)
    {
        return fields.image.has_value() ? Status{}
                                        : Fail(ErrorKind::kInvalidConfig, "Write/TestWrite plans must carry an image");
    }
    if (!fields.erase_regions.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "Read plans must not declare erase regions");
    }
    if (fields.image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Read plans must not carry an image");
    }
    return {};
}

// Places the write image in its memory map, consuming the map.
Result<std::optional<memory::MemoryImage>> PlaceImage(FlashPlanFields& fields)
{
    if (!fields.image_map.has_value())
    {
        return std::nullopt;
    }
    if (!fields.image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "only a write image can have a memory map");
    }
    auto placed = memory::MemoryImage::Create(*std::move(fields.image_map), *fields.image);
    fields.image_map.reset();
    if (!placed.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("the image does not fit its memory map: {}", placed.error().detail));
    }
    return std::optional{std::move(*placed)};
}

Status ValidateKernel(const FlashPlanFields& fields)
{
    if (!fields.kernel.has_value())
    {
        return fields.family_plan.RequiresKernel() ? Fail(ErrorKind::kInvalidConfig, "family requires a kernel image")
                                                   : Status{};
    }
    if (fields.kernel->id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel id must not be empty");
    }
    if (fields.kernel->bytes.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel bytes must not be empty");
    }
    if (static_cast<std::uint64_t>(fields.kernel->load_address) + fields.kernel->bytes.size() >
        static_cast<std::uint64_t>(0xffffffffU))
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel upload range overflows a 32-bit address space");
    }
    return {};
}

bool HasDuplicateConfirmation(const FlashPlanFields& fields)
{
    std::unordered_set<ConfirmationSpec::Id> seen_ids;
    return std::ranges::any_of(fields.confirmations, [&seen_ids](const ConfirmationSpec& confirmation)
                               { return !seen_ids.insert(confirmation.id).second; });
}
} // namespace

Result<FlashPlan> ValidateAndBuild(FlashPlanFields fields)
{
    auto rom_image = ValidateTargetAndRegions(fields)
                         .and_then([&] { return ValidateOperationShape(fields); })
                         .and_then([&] { return PlaceImage(fields); });
    if (!rom_image.has_value())
    {
        return std::unexpected(rom_image.error());
    }
    if (auto valid = ValidateKernel(fields); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // The plan's family and transport are the facts its family plan recorded
    // (ADR 0030), so an empty family plan leaves the plan with neither.
    if (!fields.family_plan.HasValue())
    {
        return Fail(ErrorKind::kInvalidConfig, "family_plan must not be empty");
    }
    if (HasDuplicateConfirmation(fields))
    {
        return Fail(ErrorKind::kInvalidConfig, "duplicate confirmation id declared");
    }
    const std::uint64_t total_transfer_bytes = fields.transfer_region.length;
    return FlashPlan(std::move(fields), total_transfer_bytes, *std::move(rom_image));
}

} // namespace fastecu::flash
