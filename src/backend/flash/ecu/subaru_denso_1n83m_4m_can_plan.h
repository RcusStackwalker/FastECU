#pragma once

#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruDenso1n83m4mCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDenso1n83m4mCanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDenso1n83m4mCanProtocols();
} // namespace fastecu::flash
