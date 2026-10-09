#pragma once

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruHitachiM32rKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruHitachiM32rKlinePlan(const FlashPlan& plan);
} // namespace fastecu::flash
