#pragma once

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruUnisiaJecsPlan(FlashOperation operation, std::string_view protocol_name,
                                            std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruUnisiaJecsPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruUnisiaJecsPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruUnisiaJecsPlan(plan);
    }
};
} // namespace fastecu::flash
