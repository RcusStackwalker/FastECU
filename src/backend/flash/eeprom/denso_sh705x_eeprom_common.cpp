// src/backend/flash/eeprom/denso_sh705x_eeprom_common.cpp
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

// Literal values transcribed from src/backend/definitions/kernelmemorymodels.h
// (eblocks_SH7055/eblocks_SH7058, kblocks_SH7055/kblocks_SH7058). Do not
// derive these from anywhere else; the MCU table is the single source of
// truth the legacy code also reads from.
struct McuBounds
{
    MemoryRegion eeprom;
    MemoryRegion kernel_ram;
};

Result<McuBounds> resolve_mcu_bounds(const std::string& mcu_name)
{
    if (mcu_name == "SH7055")
    {
        return McuBounds{
            .eeprom = MemoryRegion{.start = /* eblocks_SH7055[0].start */ 0x00000000,
                                   .length = /* eblocks_SH7055[0].len */ 0x00000100},
            .kernel_ram = MemoryRegion{.start = /* kblocks_SH7055[0].start */ 0xFFFF6004,
                                       .length = /* kblocks_SH7055[0].len */ 0x00006000},
        };
    }
    if (mcu_name == "SH7058")
    {
        return McuBounds{
            .eeprom = MemoryRegion{.start = /* eblocks_SH7058[0].start */ 0x00000000,
                                   .length = /* eblocks_SH7058[0].len */ 0x00000100},
            .kernel_ram = MemoryRegion{.start = /* kblocks_SH7058[0].start */ 0xFFFF3000,
                                       .length = /* kblocks_SH7058[0].len */ 0x00009000},
        };
    }
    return fail(ErrorKind::InvalidConfig, "unknown SH705x mcu_name: " + mcu_name);
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

Result<FlashPlan> build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput input)
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
    const std::uint64_t kernel_end =
        static_cast<std::uint64_t>(input.kernel.load_address) + input.kernel.bytes.size();
    const std::uint64_t ram_end =
        static_cast<std::uint64_t>(bounds->kernel_ram.start) + bounds->kernel_ram.length;
    if (input.kernel.load_address < bounds->kernel_ram.start || kernel_end > ram_end)
    {
        return fail(ErrorKind::InvalidConfig, "kernel load range is outside the SH705x RAM region");
    }

    TransportKind transport = input.family == FlashFamily::DensoSh705xEepromKline
                                  ? TransportKind::Kline
                                  : TransportKind::CanIso15765;

    FamilyPlan family_plan;
    if (transport == TransportKind::Kline)
    {
        if (!kline_supports(input.security))
        {
            return fail(ErrorKind::InvalidConfig,
                        "security variant is not supported on the K-Line transport");
        }
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
        // OPEN QUESTION, resolve before this task is done: the legacy CAN
        // operation has a fifth crypto branch, "_ecutek_racerom_alt", which
        // calls the *same* generate_ecutek_seed_key() as plain "_ecutek"
        // (not generate_ecutek_racerom_can_seed_key(), which plain
        // "_ecutek_racerom" uses) but with an extra RAM-location
        // preprocessing step folded in via that function's own internal
        // flash_method check. DensoSecurityVariant has only four values
        // (Stock/EcuTek/Cobb/EcuTekRaceRom per the design spec verbatim) and
        // this input struct does not carry the raw flash_method string, so
        // there is currently no field this builder can populate to tell the
        // executor "this is the _alt RAM-preprocessing variant, not plain
        // EcuTek." Do not silently map "_ecutek_racerom_alt" onto
        // EcuTekRaceRom -- that selects the wrong cryptographic function.
        // Either (a) confirm with whoever owns this design that
        // "_ecutek_racerom_alt" is out of scope for 5c (the earlier
        // characterization work found read_ram_location() has a
        // pre-existing bug reading from an unpopulated buffer, so this path
        // may already be unreachable/broken on real hardware today -- if
        // so, record that explicitly in docs/flash-qualification-matrix.md's
        // notes column rather than silently dropping it), or (b) add a
        // field to DensoSh705xEepromCanPlan (e.g.
        // `bool ecutek_racerom_alt_ram_preprocessing`) and thread it through
        // here and through DensoSh705xEepromInput. Do not guess silently --
        // this is exactly the kind of behavior-preservation gap the design
        // spec's "compatibility contracts unless explicitly identified as
        // unsupported" rule is meant to catch.
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
        .family_plan = std::move(family_plan),
        .confirmations = confirmations_for_mode(input.mode),
    };
    return validate_and_build(std::move(fields));
}

} // namespace fastecu::flash
