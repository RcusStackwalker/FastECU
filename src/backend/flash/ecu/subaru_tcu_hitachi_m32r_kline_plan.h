#pragma once

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruTcuHitachiM32rKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruTcuHitachiM32rKlinePlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruTcuHitachiM32rKlinePlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruTcuHitachiM32rKlinePlan(plan);
    }
};
} // namespace fastecu::flash
