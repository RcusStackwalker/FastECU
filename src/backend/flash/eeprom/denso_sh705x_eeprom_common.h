// src/backend/flash/eeprom/denso_sh705x_eeprom_common.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Portable builder inputs. No FileActions, no paths, no platform handles --
// build_eeprom_read_plan assembles this from the parsed protocol catalog.
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
// DensoSh705xEepromInput::eeprom_region against. Exposed so
// build_eeprom_read_plan can supply a matching region without independently
// re-transcribing the kernelmemorymodels.h literals.
Result<MemoryRegion> resolve_sh705x_eeprom_region(const std::string& mcu_name);

// Transport-specific sizes derived from the raw kernel file length. K-Line
// uploads a four-byte-aligned payload followed by a separate four-byte
// checksum-bypass trailer. CAN uploads one or more complete 128-byte blocks.
// `ram_footprint_bytes` is therefore the complete address range touched on
// the ECU, while `payload_bytes` is the length each executor passes to its
// primary upload request.
struct DensoSh705xEepromUploadSizes
{
    std::uint32_t payload_bytes;
    std::uint32_t ram_footprint_bytes;
};

// The single source of truth for the family-specific upload expansion used
// by both validation and the executors. Rejects any size whose alignment or
// trailer addition cannot be represented safely as uint32_t.
Result<DensoSh705xEepromUploadSizes> denso_sh705x_eeprom_upload_sizes(
    FlashFamily family, std::size_t raw_kernel_bytes);

// Validates every Denso SH705x EEPROM plan constraint decidable from the
// supplied metadata. Pass std::nullopt before the kernel file is read: mode,
// family, MCU/EEPROM region, K-Line security, and a definitely out-of-range
// kernel load address are rejected without I/O. Pass the actual kernel byte
// count when it is available to validate the complete family-specific upload
// footprint in kernel RAM.
//
// build_denso_sh705x_eeprom_plan calls this same function with kernel_size,
// making it the single source of truth for both preflight and final plan
// validation.
Result<void> validate_denso_sh705x_eeprom_preflight(const DensoSh705xEepromInput& input,
                                                    std::optional<std::size_t> kernel_size);

Result<FlashPlan> build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput input);

} // namespace fastecu::flash
