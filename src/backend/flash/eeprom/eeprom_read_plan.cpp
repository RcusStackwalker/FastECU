#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

namespace fastecu::flash
{
namespace
{
// The exact protocol ids routed to each EEPROM family. Legacy mainwindow.cpp
// sent the "_kline" names to EepromEcuSubaruDensoSH705xKline and every other
// Denso SH705x EEPROM name ("_densocan", "_can", "_can_diesel") to
// EepromEcuSubaruDensoSH705xCan.
constexpr std::array kKlineProtocols{
    std::string_view{"sub_ecu_eeprom_denso_sh7055_kline"},
    std::string_view{"sub_ecu_eeprom_denso_sh7058_kline"},
};
constexpr std::array kCanProtocols{
    std::string_view{"sub_ecu_eeprom_denso_sh7055_densocan"},
    std::string_view{"sub_ecu_eeprom_denso_sh7058_densocan"},
    std::string_view{"sub_ecu_eeprom_denso_sh7058_can"},
    std::string_view{"sub_ecu_eeprom_denso_sh7058_can_diesel"},
};

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
Result<DensoSecurityVariant> SecurityForProtocol(std::string_view protocol_name)
{
    if (protocol_name.ends_with("_ecutek_racerom_alt"))
    {
        return Fail(ErrorKind::kInvalidConfig, "_ecutek_racerom_alt is not supported by the portable EEPROM path");
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

Result<FlashPlan> BuildEepromReadPlan(FlashFamily family, const config::ConfigPaths& paths,
                                      const config::ProtocolSpec& protocol, EepromReadMode mode,
                                      IFileRepository& file_repository)
{
    const std::string target_id(protocol.name);

    // Every fallible metadata-only validation runs before the kernel read below.
    if (!protocol.kernel_load_address.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("protocol '{}' declares no kernel load address", protocol.name));
    }
    Result<MemoryRegion> eeprom_region = ResolveSh705xEepromRegion(std::string(protocol.mcu));
    if (!eeprom_region.has_value())
    {
        return std::unexpected(eeprom_region.error());
    }
    Result<DensoSecurityVariant> security = SecurityForProtocol(protocol.name);
    if (!security.has_value())
    {
        return std::unexpected(security.error());
    }

    DensoSh705xEepromInput input{
        .operation = FlashOperation::kRead,
        .family = family,
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
    return ValidateDensoSh705xEepromPreflight(input, std::nullopt)
        .and_then(
            [&]
            {
                // KernelFileHandle preserves the configured directory's trailing-slash contract.
                return file_repository.Read(config::KernelFileHandle(paths, protocol.kernel));
            })
        .and_then(
            [&](std::vector<std::uint8_t> kernel_bytes)
            {
                input.kernel.bytes = std::move(kernel_bytes);
                return BuildDensoSh705xEepromPlan(std::move(input));
            });
}

std::span<const std::string_view> DensoSh705xEepromKlineProtocols()
{
    return kKlineProtocols;
}

std::span<const std::string_view> DensoSh705xEepromCanProtocols()
{
    return kCanProtocols;
}

} // namespace fastecu::flash
