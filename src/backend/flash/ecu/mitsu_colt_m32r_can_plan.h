#pragma once
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Builds a validated Mitsubishi Colt M32R CAN plan without I/O.
Result<FlashPlan> BuildMitsuColtM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                            std::string_view mcu_type, std::optional<bytes::Bytes> image);

// Defensive executor boundary for plans assembled without the family builder.
Status ValidateMitsuColtM32rCanPlan(const FlashPlan& plan);

} // namespace fastecu::flash
