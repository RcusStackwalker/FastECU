#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruDensoSh72543CanDieselPlan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDensoSh72543CanDieselPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDensoSh72543CanDieselPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh72543CanDieselPlan(plan);
    }
};
} // namespace fastecu::flash
