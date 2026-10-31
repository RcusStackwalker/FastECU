#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruHitachiM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruHitachiM32rCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruHitachiM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruHitachiM32rCanPlan(plan);
    }
};
// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruHitachiM32rCanProtocols();
} // namespace fastecu::flash
