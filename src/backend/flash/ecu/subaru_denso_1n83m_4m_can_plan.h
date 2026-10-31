#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruDenso1n83m4mCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDenso1n83m4mCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDenso1n83m_4mCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDenso1n83m4mCanPlan(plan);
    }
};
// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruDenso1n83m4mCanProtocols();
} // namespace fastecu::flash
