#pragma once

#include <optional>
#include <span>
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

// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruDensoSh705xDensoCanProtocols();
} // namespace fastecu::flash
