#pragma once
#include <optional>
#include <span>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Builds a validated Mitsubishi Colt M32R CAN plan without I/O.
Result<FlashPlan> BuildMitsuColtM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                            std::string_view mcu_type, std::optional<bytes::Bytes> image);

// Defensive executor boundary for plans assembled without the family builder.
Status ValidateMitsuColtM32rCanPlan(const FlashPlan& plan);

template <> struct PlanValidator<MitsuColtM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateMitsuColtM32rCanPlan(plan);
    }
};

// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> MitsuColtM32rCanProtocols();
} // namespace fastecu::flash
