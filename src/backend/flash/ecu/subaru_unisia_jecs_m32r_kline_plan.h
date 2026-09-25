#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Exact protocol/MCU pairs sub_ecu_unisia_jecs_{20,30,40,70}; _40 and _70 are
// read-only. A Write plan carries ApplyProgrammingVoltage unless
// `adapter_supplies_programming_voltage`; the desktop workflow collects that
// confirmation before the executor starts.
Result<FlashPlan> build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                           bool adapter_supplies_programming_voltage);
Status validate_subaru_unisia_jecs_m32r_kline_plan(const FlashPlan& plan);
} // namespace fastecu::flash
