#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruDensoSh705502Plan(FlashOperation operation, std::string_view protocol_name,
                                               std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                               KernelImage kernel);
Status ValidateSubaruDensoSh705502Plan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh705502Protocols();
} // namespace fastecu::flash
