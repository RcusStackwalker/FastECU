#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> build_subaru_hitachi_sh7058_plan(FlashOperation operation, std::string_view protocol,
                                                   std::string_view mcu, std::optional<bytes::Bytes> image);
Status validate_subaru_hitachi_sh7058_plan(const FlashPlan& plan);
} // namespace fastecu::flash
