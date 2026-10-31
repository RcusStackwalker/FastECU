// src/backend/flash/eeprom/denso_sh705x_eeprom_common.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
enum class DensoSecurityVariant
{
    kStock,
    kEcuTek,
    kCobb,
    kEcuTekRaceRom,
};

enum class EepromReadMode : std::uint8_t
{
    kMode2 = 2,
    kMode3 = 3,
    kMode4 = 4,
};

struct DensoSh705xEepromKlinePlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint8_t tester_id; // 0xf0 for the current family
    std::uint8_t target_id; // 0x10 for the current family; unrelated to
                            // FlashPlan::target_id(), which names the
                            // selected protocol/configuration string, not
                            // this numeric M32R/SH705x bus address.
    int initial_baud;       // 4800
    int kernel_baud;        // snapshotted resolved family value
};

template <> struct FamilyTraits<DensoSh705xEepromKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kDensoSh705xEepromKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<DensoSh705xEepromKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

struct DensoSh705xEepromCanPlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false
};

template <> struct FamilyTraits<DensoSh705xEepromCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kDensoSh705xEepromCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<DensoSh705xEepromCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// Portable builder inputs. No FileActions, no paths, no platform handles --
// BuildEepromReadPlan assembles this from the parsed protocol catalog.
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
// "SH7058") from the same MCU table BuildDensoSh705xEepromPlan validates
// DensoSh705xEepromInput::eeprom_region against. Exposed so
// BuildEepromReadPlan can supply a matching region without independently
// re-transcribing the kernelmemorymodels.h literals.
Result<MemoryRegion> ResolveSh705xEepromRegion(const std::string& mcu_name);

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
Result<DensoSh705xEepromUploadSizes> ComputeDensoSh705xEepromUploadSizes(FlashFamily family,
                                                                         std::size_t raw_kernel_bytes);

// Validates every Denso SH705x EEPROM plan constraint decidable from the
// supplied metadata. Pass std::nullopt before the kernel file is read: mode,
// family, MCU/EEPROM region, K-Line security, and a definitely out-of-range
// kernel load address are rejected without I/O. Pass the actual kernel byte
// count when it is available to validate the complete family-specific upload
// footprint in kernel RAM.
//
// BuildDensoSh705xEepromPlan calls this same function with kernel_size,
// making it the single source of truth for both preflight and final plan
// validation.
Result<void> ValidateDensoSh705xEepromPreflight(const DensoSh705xEepromInput& input,
                                                std::optional<std::size_t> kernel_size);

Result<FlashPlan> BuildDensoSh705xEepromPlan(DensoSh705xEepromInput input);

// Re-runs the builder's constraints (ValidateDensoSh705xEepromPreflight, with
// the kernel's byte count) on a finished plan, so an executor refuses a plan
// the builder's preflight would refuse before any I/O. It does not re-check
// what the builder hard-codes (wire parameters, per-mode confirmations).
Status ValidateDensoSh705xEepromPlan(const FlashPlan& plan);

} // namespace fastecu::flash
