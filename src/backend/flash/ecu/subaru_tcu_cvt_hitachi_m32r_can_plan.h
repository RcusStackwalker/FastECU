#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruTcuCvtHitachiM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruTcuCvtHitachiM32rCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<SubaruTcuCvtHitachiM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruTcuCvtHitachiM32rCanPlan(plan);
    }
};
// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruTcuCvtHitachiM32rCanProtocols();
} // namespace fastecu::flash
