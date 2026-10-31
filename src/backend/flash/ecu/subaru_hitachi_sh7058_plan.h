#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruHitachiSh7058Plan(FlashOperation operation, std::string_view protocol,
                                               std::string_view mcu, std::optional<bytes::Bytes> image);
Status ValidateSubaruHitachiSh7058Plan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruHitachiSh7058Protocols();
} // namespace fastecu::flash
