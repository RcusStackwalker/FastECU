#pragma once
#include <optional>
#include <span>
#include <string_view>

#include "src/algorithms/memory/memory_image.h"
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// A Write/TestWrite `image` is the ROM file placed by its memory map; every
// flash block of the MCU must be writable ROM file bytes in it, whatever the
// file's size (ADR 0020).
Result<FlashPlan> BuildSubaruDensoMc68hc16y502Plan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<memory::MemoryImage> image,
                                                   KernelImage kernel);
Status ValidateSubaruDensoMc68hc16y502Plan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDensoMc68hc16y5_02Plan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoMc68hc16y502Plan(plan);
    }
};
// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoMc68hc16y502Protocols();
} // namespace fastecu::flash
