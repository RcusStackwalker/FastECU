#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruHitachiSh72543rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                    std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruHitachiSh72543rCanPlan(const FlashPlan& plan);

} // namespace fastecu::flash
