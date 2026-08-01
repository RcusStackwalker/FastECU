// src/backend/flash/eeprom/denso_sh705x_eeprom_common.cpp
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include <format>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{

// Literal values transcribed from src/backend/definitions/kernelmemorymodels.h
// (eblocks_SH7055[0], line 279-281; eblocks_SH7058[0], line 221-223). Do not
// derive these from anywhere else; the MCU table is the single source of
// truth both this function and resolve_mcu_bounds() below read from -- and
// the only place these two literals are written (build_eeprom_read_plan calls
// this function directly rather than keeping its own copy).
Result<MemoryRegion> resolve_sh705x_eeprom_region(const std::string& mcu_name)
{
    if (mcu_name == "SH7055")
    {
        return MemoryRegion{.start = /* eblocks_SH7055[0].start */ 0x00000000,
                            .length = /* eblocks_SH7055[0].len */ 0x00000100};
    }
    if (mcu_name == "SH7058")
    {
        return MemoryRegion{.start = /* eblocks_SH7058[0].start */ 0x00000000,
                            .length = /* eblocks_SH7058[0].len */ 0x00000100};
    }
    return fail(ErrorKind::InvalidConfig, std::format("unknown SH705x mcu_name: {}", mcu_name));
}

namespace
{

// Literal values transcribed from src/backend/definitions/kernelmemorymodels.h
// (kblocks_SH7055/kblocks_SH7058). Do not derive these from anywhere else;
// the MCU table is the single source of truth the legacy code also reads
// from.
struct McuBounds
{
    MemoryRegion eeprom;
    MemoryRegion kernel_ram;
};

Result<McuBounds> resolve_mcu_bounds(const std::string& mcu_name)
{
    Result<MemoryRegion> eeprom = resolve_sh705x_eeprom_region(mcu_name);
    if (!eeprom.has_value())
    {
        return std::unexpected(eeprom.error());
    }
    if (mcu_name == "SH7055")
    {
        return McuBounds{
            .eeprom = *eeprom,
            .kernel_ram = MemoryRegion{.start = /* kblocks_SH7055[0].start */ 0xFFFF6004,
                                       .length = /* kblocks_SH7055[0].len */ 0x00006000},
        };
    }
    if (mcu_name == "SH7058")
    {
        return McuBounds{
            .eeprom = *eeprom,
            .kernel_ram = MemoryRegion{.start = /* kblocks_SH7058[0].start */ 0xFFFF3000,
                                       .length = /* kblocks_SH7058[0].len */ 0x00009000},
        };
    }
    return fail(ErrorKind::InvalidConfig, std::format("unknown SH705x mcu_name: {}", mcu_name));
}

bool kline_supports(DensoSecurityVariant security)
{
    // The legacy K-Line operation only branches on flash_method.endsWith("_ecutek")
    // vs. everything else -- there is no Cobb or RaceRom K-Line path.
    return security == DensoSecurityVariant::Stock || security == DensoSecurityVariant::EcuTek;
}

std::vector<ConfirmationSpec> confirmations_for_mode(EepromReadMode mode)
{
    if (mode == EepromReadMode::Mode2)
    {
        return {
            ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead},
            ConfirmationSpec{.id = ConfirmationSpec::Id::InspectEepromBytes},
        };
    }
    return {
        ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead},
        ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition},
        ConfirmationSpec{.id = ConfirmationSpec::Id::InspectEepromBytes},
    };
}

} // namespace

Result<void> validate_denso_sh705x_eeprom_preflight(const DensoSh705xEepromInput& input,
                                                    std::optional<std::size_t> kernel_size)
{
    if (input.family != FlashFamily::DensoSh705xEepromKline &&
        input.family != FlashFamily::DensoSh705xEepromCan)
    {
        return fail(ErrorKind::InvalidConfig, "unsupported family for Denso SH705x EEPROM builder");
    }
    if (input.operation != FlashOperation::Read)
    {
        // Matches the current, intentional legacy routing: write/test_write
        // for these two families reach an operation whose write call is
        // commented out. 5c does not legitimize that as portable flashing.
        return fail(ErrorKind::Unsupported,
                    "Denso SH705x EEPROM write/test_write is not implemented");
    }
    if (input.mode != EepromReadMode::Mode2 && input.mode != EepromReadMode::Mode3 &&
        input.mode != EepromReadMode::Mode4)
    {
        return fail(ErrorKind::InvalidConfig, "EEPROM mode must be 2, 3, or 4");
    }

    Result<McuBounds> bounds = resolve_mcu_bounds(input.mcu_name);
    if (!bounds.has_value())
    {
        return std::unexpected(bounds.error());
    }
    if (input.eeprom_region.start != bounds->eeprom.start ||
        input.eeprom_region.length != bounds->eeprom.length)
    {
        return fail(ErrorKind::InvalidConfig,
                    "eeprom_region does not match the resolved MCU table entry");
    }
    const std::uint64_t ram_end =
        static_cast<std::uint64_t>(bounds->kernel_ram.start) + bounds->kernel_ram.length;
    if (input.kernel.load_address < bounds->kernel_ram.start ||
        input.kernel.load_address > ram_end ||
        (kernel_size.has_value() &&
         static_cast<std::uint64_t>(input.kernel.load_address) + *kernel_size > ram_end))
    {
        return fail(ErrorKind::InvalidConfig, "kernel load range is outside the SH705x RAM region");
    }

    if (input.family == FlashFamily::DensoSh705xEepromKline && !kline_supports(input.security))
    {
        return fail(ErrorKind::InvalidConfig,
                    "security variant is not supported on the K-Line transport");
    }

    return {};
}

Result<FlashPlan> build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput input)
{
    Result<void> preflight =
        validate_denso_sh705x_eeprom_preflight(input, input.kernel.bytes.size());
    if (!preflight.has_value())
    {
        return std::unexpected(preflight.error());
    }

    TransportKind transport = input.family == FlashFamily::DensoSh705xEepromKline
                                  ? TransportKind::Kline
                                  : TransportKind::CanIso15765;

    FamilyPlan family_plan;
    if (transport == TransportKind::Kline)
    {
        family_plan = DensoSh705xEepromKlinePlan{
            .mode = input.mode,
            .security = input.security,
            .tester_id = 0xf0,
            .target_id = 0x10,
            .initial_baud = 4800,
            .kernel_baud = 15625, // resolved family value; see upload_kernel() characterization
        };
    }
    else
    {
        // RESOLVED by Task 9 (see denso_sh705x_eeprom_can_executor.cpp's long
        // comment above DensoSh705xEepromCanExecutor::connect_bootloader(),
        // and task-9-report.md): this was an OPEN QUESTION about the legacy
        // CAN operation's fifth crypto branch, "_ecutek_racerom_alt", which
        // calls the *same* generate_ecutek_seed_key() as plain "_ecutek"
        // (not generate_ecutek_racerom_can_seed_key(), which plain
        // "_ecutek_racerom" uses) but with an extra RAM-location
        // preprocessing step folded in via that function's own internal
        // flash_method check.
        //
        // Resolution: option (a) from the original comment -- this path is
        // out of scope for step 5c. DensoSecurityVariant deliberately keeps
        // only its four values (Stock/EcuTek/Cobb/EcuTekRaceRom); no field
        // was added to represent "_ecutek_racerom_alt" here or on
        // DensoSh705xEepromCanPlan/DensoSh705xEepromInput. Two independent
        // reasons: (1) the RAM-preprocessing step needs a temporary
        // K-Line-shaped exchange multiplexed over the same physical adapter
        // the plan declares as CAN -- a transport-architecture capability
        // that does not exist anywhere in the portable seam yet, well beyond
        // a per-security-variant plan field; (2) Task 7's characterization
        // work could only pin read_ram_location()'s FAILURE path -- its
        // "success" path reads from a buffer that is never populated
        // anywhere in the legacy function, so it is undefined behavior on
        // real hardware, not a stable, reproducible contract, and there is no
        // golden trace for the resulting seed key to reproduce. Do NOT
        // silently map "_ecutek_racerom_alt" onto EcuTekRaceRom -- that would
        // select the wrong cryptographic function.
        family_plan = DensoSh705xEepromCanPlan{
            .mode = input.mode,
            .security = input.security,
            .request_id = 0x7e0,
            .response_id = 0x7e8,
            .bitrate = 500000,
            .extended_id = false,
        };
    }

    FlashPlanFields fields{
        .operation = FlashOperation::Read,
        .family = input.family,
        .transport = transport,
        .target_id = input.target_id,
        .mcu_name = input.mcu_name,
        .transfer_region = input.eeprom_region,
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::move(input.kernel),
        .family_plan = family_plan,
        .confirmations = confirmations_for_mode(input.mode),
    };
    return validate_and_build(std::move(fields));
}

} // namespace fastecu::flash
