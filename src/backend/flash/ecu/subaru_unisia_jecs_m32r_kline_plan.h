#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Exact protocol/MCU pairs sub_ecu_unisia_jecs_{20,30,40,70}, plus Read on
// sub_ecu_unisia_jecs_{20,30}_bootmode; _40, _70 and both _bootmode names are
// read-only here. A Write plan carries ApplyProgrammingVoltage unless
// `adapter_supplies_programming_voltage`; the desktop workflow collects that
// confirmation before the executor starts.
Result<FlashPlan> build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                           bool adapter_supplies_programming_voltage);
Status validate_subaru_unisia_jecs_m32r_kline_plan(const FlashPlan& plan);
} // namespace fastecu::flash
