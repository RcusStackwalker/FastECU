#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruDensoSh7058CanDieselPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                      KernelImage kernel);
Status ValidateSubaruDensoSh7058CanDieselPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruDensoSh7058CanDieselPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh7058CanDieselPlan(plan);
    }
};

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh7058CanDieselProtocols();
} // namespace fastecu::flash
