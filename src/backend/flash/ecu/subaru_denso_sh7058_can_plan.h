#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

Result<FlashPlan> BuildSubaruDensoSh7058CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                KernelImage kernel);
Status ValidateSubaruDensoSh7058CanPlan(const FlashPlan& plan);

} // namespace fastecu::flash
