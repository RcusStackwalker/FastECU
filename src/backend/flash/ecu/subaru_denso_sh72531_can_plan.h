#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruDensoSh72531CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDensoSh72531CanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDensoSh72531CanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh72531CanPlan(plan);
    }
};
// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruDensoSh72531CanProtocols();
} // namespace fastecu::flash
