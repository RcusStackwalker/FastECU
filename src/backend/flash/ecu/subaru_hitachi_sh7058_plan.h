#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruHitachiSh7058Plan(FlashOperation operation, std::string_view protocol,
                                               std::string_view mcu, std::optional<bytes::Bytes> image);
Status ValidateSubaruHitachiSh7058Plan(const FlashPlan& plan);
} // namespace fastecu::flash
