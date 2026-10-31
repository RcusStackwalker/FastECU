#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruDensoSh7058CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                KernelImage kernel);
Status ValidateSubaruDensoSh7058CanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDensoSh7058CanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh7058CanPlan(plan);
    }
};

// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruDensoSh7058CanProtocols();
} // namespace fastecu::flash
