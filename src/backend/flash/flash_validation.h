#pragma once
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// The sole construction path for FlashPlan. Performs every check provable
// from the fields alone, with zero transport/filesystem/thread interaction.
// Family builders (denso_sh705x_eeprom_common and later per-family tail
// builders) perform their own family-specific checks first, then delegate
// here for the checks that apply to every family.
Result<FlashPlan> validate_and_build(FlashPlanFields fields);

} // namespace fastecu::flash
