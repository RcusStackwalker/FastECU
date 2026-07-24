// src/platform/desktop/common/flash/flash_snapshot_adapter.cpp
#include "src/platform/desktop/common/flash/flash_snapshot_adapter.h"

#include <utility>

namespace fastecu::flash
{
namespace
{

// Mirrors mainwindow.cpp's EEPROM dispatch (lines 1257-1286): protocol names
// ending "_kline" route to EepromEcuSubaruDensoSH705xKline; every other
// Denso SH705x EEPROM protocol name in that block ("_densocan", "_can",
// "_can_diesel") routes to EepromEcuSubaruDensoSH705xCan. This adapter only
// ever builds a plan for one of those two families, so a substring check on
// "kline" (rather than reproducing every full protocol-name literal) is
// sufficient and matches the legacy branch structure.
FlashFamily family_for_protocol(const std::string& protocol_name)
{
    if (protocol_name.find("kline") != std::string::npos)
    {
        return FlashFamily::DensoSh705xEepromKline;
    }
    return FlashFamily::DensoSh705xEepromCan;
}

// CONFIRMED (not the brief's guess): the Denso security-variant suffix lives
// directly on FlashMethod. mainwindow.cpp copies
// configValues->flash_protocol_selected_protocol_name -- which already
// carries "_ecutek"/"_cobb"/"_ecutek_racerom"/"_ecutek_racerom_alt" straight
// from the protocol's XML `name` attribute in
// resources/shared/config/protocols.cfg (e.g. line 385
// "sub_ecu_denso_sh7058_can_ecutek_racerom") -- verbatim into
// ecuCalDef[rom_number]->FlashMethod (src/ui/desktop/mainwindow.cpp:1119,
// 1145). Every legacy Denso operation class reads the same suffix off its
// own `flash_method` member, itself assigned from ecuCalDef->FlashMethod
// (e.g. flash_ecu_subaru_denso_sh7058_can_operation.cpp:35,296,427-439) --
// there is no separate EcuCalDefStructure field for this. No currently
// registered Denso SH705x EEPROM protocol in protocols.cfg actually carries
// one of these suffixes today, but the branch mirrors the convention used
// everywhere else in the codebase in case a suffixed EEPROM protocol is
// added later.
Result<DensoSecurityVariant> security_for_flash_method(const std::string& flash_method)
{
    if (flash_method.ends_with("_cobb"))
    {
        return DensoSecurityVariant::Cobb;
    }
    if (flash_method.ends_with("_ecutek_racerom") || flash_method.ends_with("_ecutek_racerom_alt"))
    {
        return DensoSecurityVariant::EcuTekRaceRom;
    }
    if (flash_method.ends_with("_ecutek"))
    {
        return DensoSecurityVariant::EcuTek;
    }
    return DensoSecurityVariant::Stock;
}

// CONFIRMED: KernelStartAddr is always a "0x"-prefixed hex string in
// resources/shared/config/protocols.cfg (e.g. line 922
// "<kernel_addr>0xFFFF6004</kernel_addr>" for
// sub_ecu_eeprom_denso_sh7055_kline), and every legacy call site parses it
// the same way -- QString::toUInt(&ok, 16) -- e.g.
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp:78,
// flash_ecu_subaru_denso_sh7058_can_operation.cpp:78, and five more
// sibling operation classes. Qt's QString::toUInt accepts the "0x"/"0X"
// prefix when base is 16, so this matches both prefixed and bare-hex-digit
// strings; it fails (ok=false) on anything else, including decimal text.
Result<std::uint32_t> parse_kernel_start_addr(const QString& kernel_start_addr)
{
    bool ok = false;
    const std::uint32_t value = kernel_start_addr.toUInt(&ok, 16);
    if (!ok)
    {
        return fail(ErrorKind::InvalidConfig,
                    "KernelStartAddr did not parse as an address: " +
                        kernel_start_addr.toStdString());
    }
    return value;
}

} // namespace

LegacyFlashSnapshotAdapter::LegacyFlashSnapshotAdapter(IFileRepository& file_repository)
    : file_repository_(file_repository)
{
}

Result<FlashPlan> LegacyFlashSnapshotAdapter::build_read_plan(
    const FileActions::EcuCalDefStructure& ecu_cal_def, const std::string& protocol_name,
    EepromReadMode mode, const std::string& kernel_file_handle) const
{
    const std::string mcu_name = ecu_cal_def.McuType.toStdString();
    const std::string flash_method = ecu_cal_def.FlashMethod.toStdString();

    // Every fallible setup step below runs before file_repository_.read() is
    // ever called, so an invalid snapshot is rejected without I/O.
    Result<MemoryRegion> eeprom_region = resolve_sh705x_eeprom_region(mcu_name);
    if (!eeprom_region.has_value())
    {
        return std::unexpected(eeprom_region.error());
    }
    Result<std::uint32_t> kernel_start_addr = parse_kernel_start_addr(ecu_cal_def.KernelStartAddr);
    if (!kernel_start_addr.has_value())
    {
        return std::unexpected(kernel_start_addr.error());
    }
    Result<DensoSecurityVariant> security = security_for_flash_method(flash_method);
    if (!security.has_value())
    {
        return std::unexpected(security.error());
    }

    Result<std::vector<std::uint8_t>> kernel_bytes = file_repository_.read(kernel_file_handle);
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }

    return build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = family_for_protocol(protocol_name),
        .target_id = protocol_name,
        .mcu_name = mcu_name,
        .flash_method = flash_method,
        .kernel = KernelImage{
            .id = protocol_name + "-kernel",
            .load_address = *kernel_start_addr,
            .bytes = std::move(*kernel_bytes),
        },
        .mode = mode,
        .security = *security,
        .eeprom_region = *eeprom_region,
    });
}

} // namespace fastecu::flash
