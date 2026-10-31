#pragma once

#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruTcuCvtMitsuMh8104CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruTcuCvtMitsuMh8104CanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruTcuCvtMitsuMh8104CanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruTcuCvtMitsuMh8104CanPlan(plan);
    }
};
// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruTcuCvtMitsuMh8104CanProtocols();
} // namespace fastecu::flash
