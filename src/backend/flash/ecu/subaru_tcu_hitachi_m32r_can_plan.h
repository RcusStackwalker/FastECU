#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruTcuHitachiM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruTcuHitachiM32rCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruTcuHitachiM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruTcuHitachiM32rCanPlan(plan);
    }
};

} // namespace fastecu::flash
