#pragma once

#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruHitachiM32rKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruHitachiM32rKlinePlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruHitachiM32rKlinePlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruHitachiM32rKlinePlan(plan);
    }
};
// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruHitachiM32rKlineProtocols();
} // namespace fastecu::flash
