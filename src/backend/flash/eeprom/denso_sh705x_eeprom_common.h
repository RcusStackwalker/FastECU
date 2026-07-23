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

Result<FlashPlan> build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput input);

} // namespace fastecu::flash
