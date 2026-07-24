// src/backend/flash/eeprom/denso_sh705x_eeprom_common.h
#pragma once
#include <string>

#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Portable builder inputs. No FileActions, no paths, no platform handles --
// LegacyFlashSnapshotAdapter (desktop) assembles this from legacy state and
// its step-5d successor assembles it from a parsed, portable definition.
struct DensoSh705xEepromInput
{
    FlashOperation operation;
    FlashFamily family; // DensoSh705xEepromKline or DensoSh705xEepromCan
    std::string target_id;
    std::string mcu_name;
    std::string flash_method;
    KernelImage kernel;
    EepromReadMode mode;
    DensoSecurityVariant security;
    MemoryRegion eeprom_region;
};

// Resolves the fixed SH705x EEPROM address range for mcu_name ("SH7055" or
// "SH7058") from the same MCU table build_denso_sh705x_eeprom_plan validates
// DensoSh705xEepromInput::eeprom_region against. Exposed so callers that
// assemble a DensoSh705xEepromInput from platform state (e.g.
// LegacyFlashSnapshotAdapter, step 5c Task 14) can supply a matching region
// without independently re-transcribing the kernelmemorymodels.h literals.
Result<MemoryRegion> resolve_sh705x_eeprom_region(const std::string& mcu_name);

Result<FlashPlan> build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput input);

} // namespace fastecu::flash
