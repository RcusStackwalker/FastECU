#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruTcuDensoSh705xCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                   KernelImage kernel);
Status ValidateSubaruTcuDensoSh705xCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruTcuDensoSh705xCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruTcuDensoSh705xCanPlan(plan);
    }
};

// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruTcuDensoSh705xCanProtocols();
} // namespace fastecu::flash
