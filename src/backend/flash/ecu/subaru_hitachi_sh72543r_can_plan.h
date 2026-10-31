#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruHitachiSh72543rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                    std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruHitachiSh72543rCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruHitachiSh72543rCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruHitachiSh72543rCanPlan(plan);
    }
};
// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruHitachiSh72543rCanProtocols();
} // namespace fastecu::flash
