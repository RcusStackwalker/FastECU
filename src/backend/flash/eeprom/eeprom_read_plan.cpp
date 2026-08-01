#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <format>
#include <limits>
#include <string>
#include <utility>

#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/protocol_catalog.h"
#include "src/backend/definition/text_format.h"
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
    if (protocol_name.find("kline") != std::string_view::npos)
    {
        return FlashFamily::DensoSh705xEepromKline;
    }
    return FlashFamily::DensoSh705xEepromCan;
}

// CONFIRMED: the Denso security-variant suffix lives directly on the
// protocol name. mainwindow.cpp copies
// configValues->flash_protocol_selected_protocol_name -- which already
// carries "_ecutek"/"_cobb"/"_ecutek_racerom"/"_ecutek_racerom_alt" straight
// from the protocol's XML `name` attribute in
// resources/shared/config/protocols.cfg (e.g. line 385
// "sub_ecu_denso_sh7058_can_ecutek_racerom") -- verbatim into
// ecuCalDef[rom_number]->FlashMethod (src/ui/desktop/mainwindow.cpp:1136,
// 1162). Every legacy Denso operation class reads the same suffix off its
// own `flash_method` member. No currently registered Denso SH705x EEPROM
// protocol carries one of these suffixes today, but the branch mirrors the
// convention used everywhere else in the codebase in case a suffixed EEPROM
// protocol is added later.
DensoSecurityVariant security_for_protocol(std::string_view protocol_name)
{
    if (protocol_name.ends_with("_cobb"))
    {
        return DensoSecurityVariant::Cobb;
    }
    if (protocol_name.ends_with("_ecutek_racerom") ||
        protocol_name.ends_with("_ecutek_racerom_alt"))
    {
        return DensoSecurityVariant::EcuTekRaceRom;
    }
    if (protocol_name.ends_with("_ecutek"))
    {
        return DensoSecurityVariant::EcuTek;
    }
    return DensoSecurityVariant::Stock;
}

// KernelStartAddr is a "0x"-prefixed hex string in protocols.cfg (e.g. line
// 922 "<kernel_addr>0xFFFF6004</kernel_addr>"). Legacy parsed it with
// QString::toUInt(&ok, 16); definition::parse_hex_value is the portable
// equivalent, pinned against Qt by tests/test_hex_parse_qt_compat.cpp. It
// returns uint64, so the uint32 bound Qt enforced by overflow is enforced
// explicitly here.
Result<std::uint32_t> parse_kernel_start_addr(std::string_view kernel_addr)
{
    const auto parsed = definition::parse_hex_value(kernel_addr);
    if (!parsed.has_value() || *parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("kernel_addr did not parse as a 32-bit address: '{}'",
                                kernel_addr));
    }
    return static_cast<std::uint32_t>(*parsed);
}

Result<config::ProtocolEntry> resolve_protocol(const config::ConfigPaths& paths,
                                               std::string_view protocol_name,
                                               IFileRepository& file_repository)
{
    Result<config::ProtocolCatalog> protocols = config::load_protocol_catalog(paths, file_repository);
    if (!protocols.has_value())
    {
        return std::unexpected(protocols.error());
    }
    Result<config::CarModelCatalog> car_models =
        config::load_car_model_catalog(paths, file_repository);
    if (!car_models.has_value())
    {
        return std::unexpected(car_models.error());
    }

    const std::vector<config::ResolvedCarModel> resolved =
        config::resolve_car_models(*protocols, *car_models);
    const std::optional<std::size_t> index =
        config::find_car_model_by_protocol_name(resolved, protocol_name);
    if (!index.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("no car model references protocol '{}'", protocol_name));
    }
    const config::ResolvedCarModel& row = resolved[*index];
    if (!row.protocol.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("protocol '{}' is referenced by a car model but absent "
                                "from the <protocols> section",
                                protocol_name));
    }
    return *row.protocol;
}

} // namespace

Result<FlashPlan> build_eeprom_read_plan(const config::ConfigPaths& paths,
                                         std::string_view protocol_name,
                                         EepromReadMode mode,
                                         IFileRepository& file_repository)
{
    Result<config::ProtocolEntry> entry = resolve_protocol(paths, protocol_name, file_repository);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }

    // Every fallible non-I/O step runs before the kernel read below.
    Result<std::uint32_t> kernel_start_addr = parse_kernel_start_addr(entry->kernel_addr);
    if (!kernel_start_addr.has_value())
    {
        return std::unexpected(kernel_start_addr.error());
    }
    Result<MemoryRegion> eeprom_region = resolve_sh705x_eeprom_region(entry->mcu);
    if (!eeprom_region.has_value())
    {
        return std::unexpected(eeprom_region.error());
    }

    // No separator inserted: kernel_files_directory already carries its
    // trailing separator (config_paths.cpp:20 builds it as base +
    // "/kernels/"), exactly as mainwindow.cpp:1137 concatenated it. Adding
    // one produces a doubled separator and a failed open.
    const std::string kernel_handle = paths.kernel_files_directory + entry->kernel;
    Result<std::vector<std::uint8_t>> kernel_bytes = file_repository.read(kernel_handle);
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }

    const std::string target_id(protocol_name);
    return build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = family_for_protocol(protocol_name),
        .target_id = target_id,
        .mcu_name = entry->mcu,
        .flash_method = target_id,
        .kernel = KernelImage{
            .id = target_id + "-kernel",
            .load_address = *kernel_start_addr,
            .bytes = std::move(*kernel_bytes),
        },
        .mode = mode,
        .security = security_for_protocol(protocol_name),
        .eeprom_region = *eeprom_region,
    });
}

} // namespace fastecu::flash
