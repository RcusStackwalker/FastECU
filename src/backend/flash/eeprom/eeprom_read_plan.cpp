#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

namespace fastecu::flash
{
namespace
{

// Mirrors mainwindow.cpp's EEPROM dispatch (lines 1257-1286): protocol names
// ending "_kline" route to EepromEcuSubaruDensoSH705xKline; every other
// Denso SH705x EEPROM protocol name in that block ("_densocan", "_can",
// "_can_diesel") routes to EepromEcuSubaruDensoSH705xCan. This use case only
// ever builds a plan for one of those two families, so a substring check on
// "kline" (rather than reproducing every full protocol-name literal) is
// sufficient and matches the legacy branch structure.
FlashFamily family_for_protocol(std::string_view protocol_name)
{
    if (protocol_name.contains("kline"))
    {
        return FlashFamily::kDensoSh705xEepromKline;
    }
    return FlashFamily::kDensoSh705xEepromCan;
}

// CONFIRMED: the Denso security-variant suffix lives directly on the
// protocol name. mainwindow.cpp copies
// the selected vehicle's protocol_name -- which already
// carries "_ecutek"/"_cobb"/"_ecutek_racerom"/"_ecutek_racerom_alt" straight
// from the protocol's name in the built-in catalog
// (src/backend/config/builtin_catalog.cpp, e.g.
// "sub_ecu_denso_sh7058_can_ecutek_racerom") -- verbatim into
// ecuCalDef[rom_number]->FlashMethod (src/ui/desktop/mainwindow.cpp:1136,
// 1162). Every legacy Denso operation class reads the same suffix off its
// own `flash_method` member. No currently registered Denso SH705x EEPROM
// protocol carries one of these suffixes today, but the branch mirrors the
// convention used everywhere else in the codebase in case a suffixed EEPROM
// protocol is added later. The `_ecutek_racerom_alt` variant is deliberately
// rejected: its RAM preprocessing and crypto path are not representable by
// DensoSecurityVariant or the portable CAN executor.
Result<DensoSecurityVariant> security_for_protocol(std::string_view protocol_name)
{
    if (protocol_name.ends_with("_ecutek_racerom_alt"))
    {
        return fail(ErrorKind::kInvalidConfig, "_ecutek_racerom_alt is not supported by the portable EEPROM path");
    }
    if (protocol_name.ends_with("_cobb"))
    {
        return DensoSecurityVariant::kCobb;
    }
    if (protocol_name.ends_with("_ecutek_racerom"))
    {
        return DensoSecurityVariant::kEcuTekRaceRom;
    }
    if (protocol_name.ends_with("_ecutek"))
    {
        return DensoSecurityVariant::kEcuTek;
    }
    return DensoSecurityVariant::kStock;
}

} // namespace

Result<FlashPlan> build_eeprom_read_plan(const config::ConfigPaths& paths, const config::ProtocolSpec& protocol,
                                         EepromReadMode mode, IFileRepository& file_repository)
{
    const std::string target_id(protocol.name);

    // Every fallible metadata-only validation runs before the kernel read below.
    if (!protocol.kernel_load_address.has_value())
    {
        return fail(ErrorKind::kInvalidConfig,
                    std::format("protocol '{}' declares no kernel load address", protocol.name));
    }
    Result<MemoryRegion> eeprom_region = resolve_sh705x_eeprom_region(std::string(protocol.mcu));
    if (!eeprom_region.has_value())
    {
        return std::unexpected(eeprom_region.error());
    }
    Result<DensoSecurityVariant> security = security_for_protocol(protocol.name);
    if (!security.has_value())
    {
        return std::unexpected(security.error());
    }

    DensoSh705xEepromInput input{
        .operation = FlashOperation::kRead,
        .family = family_for_protocol(protocol.name),
        .target_id = target_id,
        .mcu_name = std::string(protocol.mcu),
        .flash_method = target_id,
        .kernel =
            KernelImage{
                .id = target_id + "-kernel",
                .load_address = *protocol.kernel_load_address,
                .bytes = {},
            },
        .mode = mode,
        .security = *security,
        .eeprom_region = *eeprom_region,
    };
    if (Result<void> preflight = validate_denso_sh705x_eeprom_preflight(input, std::nullopt); !preflight.has_value())
    {
        return std::unexpected(preflight.error());
    }

    // No separator inserted: kernel_files_directory already carries its
    // trailing separator (config_paths.cpp:20 builds it as base +
    // "/kernels/"), exactly as mainwindow.cpp:1137 concatenated it. Adding
    // one produces a doubled separator and a failed open.
    const std::string kernel_handle = paths.kernel_files_directory + std::string(protocol.kernel);
    Result<std::vector<std::uint8_t>> kernel_bytes = file_repository.read(kernel_handle);
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }

    input.kernel.bytes = std::move(*kernel_bytes);
    return build_denso_sh705x_eeprom_plan(std::move(input));
}

} // namespace fastecu::flash
