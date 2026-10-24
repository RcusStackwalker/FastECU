#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruDensoSh705xDensocanPlan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                     KernelImage kernel);
Status ValidateSubaruDensoSh705xDensocanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDensoSh705xDensoCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh705xDensocanPlan(plan);
    }
};

} // namespace fastecu::flash
