#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Wave 7. Exact protocol/MCU pairs sub_ecu_unisia_jecs_20_bootmode /
// M32R_128KB and sub_ecu_unisia_jecs_30_bootmode / M32R_256KB, Write only
// (Read goes through the 6c-3 K-Line family). Write is two plans run as two
// attempts: the kernel upload, then erase and program. Both carry
// ApplyBootModeVoltages, collected by the desktop workflow before either runs.
Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation operation,
                                                                     std::string_view protocol_name,
                                                                     std::string_view mcu_type, bytes::Bytes kernel);
Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation operation,
                                                                      std::string_view protocol_name,
                                                                      std::string_view mcu_type,
                                                                      std::optional<bytes::Bytes> image);
// Validates a plan of either bootmode family.
Status validate_subaru_unisia_jecs_m32r_bootmode_plan(const FlashPlan& plan);
} // namespace fastecu::flash
