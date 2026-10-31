#pragma once

#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruDensoSh72531CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDensoSh72531CanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh72531CanProtocols();
} // namespace fastecu::flash
