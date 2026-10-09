#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Read returns the 0x0-0x2FFFF address-space image. Write is a kernel
// bootstrap: `kernel` is uploaded to RAM and started; the ROM is never
// written, so `rom_image` must be absent for every operation.
Result<FlashPlan> BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> rom_image,
                                                      std::optional<KernelImage> kernel);
Status ValidateSubaruDensoMc68hc16y502BdmPlan(const FlashPlan& plan);
} // namespace fastecu::flash
