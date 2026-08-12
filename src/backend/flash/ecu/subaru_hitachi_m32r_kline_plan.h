#pragma once

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> build_subaru_hitachi_m32r_kline_plan(
    FlashOperation operation, std::string_view protocol_name, std::string_view mcu_type,
    std::optional<bytes::Bytes> image);

Status validate_subaru_hitachi_m32r_kline_plan(const FlashPlan& plan);
} // namespace fastecu::flash
