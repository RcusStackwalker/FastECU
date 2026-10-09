#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruTcuDensoSh705xCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                   KernelImage kernel);
Status ValidateSubaruTcuDensoSh705xCanPlan(const FlashPlan& plan);

} // namespace fastecu::flash
