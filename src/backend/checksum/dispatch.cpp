#include "src/backend/checksum/dispatch.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh705x_diesel.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh7xxx.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_can.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_kline.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh7058.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh72543r.h"
#include "src/algorithms/checksum/checksum_tcu_mitsu_mh8104_can.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_denso_sh7055.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_hitachi_m32r_can.h"
#include "src/backend/checksum/flash_device_lookup.h"

namespace fastecu::checksum
{
namespace
{

constexpr std::uint32_t kDensoRecordSize = 17 * 12;

bool starts_with(std::string_view s, std::string_view prefix)
{
    return s.substr(0, prefix.size()) == prefix;
}

ChecksumResult denso_sh7xxx(bytes::ByteView rom, std::uint32_t area_start, std::int32_t offset = 0)
{
    ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, area_start, kDensoRecordSize, offset);
    if (result.changed())
    {
        // Mirrors legacy applyDensoSh7xxxChecksum: this algorithm's own
        // message is generic body text ("Checksums corrected"), not a family
        // title, so the display title is substituted here -- but only on the
        // Corrected path; any other status keeps the algorithm's own message
        // unmodified for its own (non-aggregated) dialog.
        result.message = "Subaru Denso SH705x Checksum";
    }
    return result;
}

ChecksumResult denso_sh705x_diesel(bytes::ByteView rom, std::uint32_t area_start)
{
    return ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(rom, area_start, kDensoRecordSize);
}

struct DispatchResult
{
    bool module_available = false;
    std::optional<ChecksumResult> result;
};

// Mirrors checksum_correction's flashMethod.startsWith(...) chain
// (file_actions.cpp:2199-2335) exactly, including its ordering: several
// prefixes overlap (e.g. "sub_ecu_denso_sh7058_can_diesel" is also a prefix
// match for "sub_ecu_denso_sh7058"), so more specific prefixes are checked
// first, exactly as the legacy if/else-if chain does.
DispatchResult dispatch_family(std::string_view flash_method, std::string_view rom_id, bytes::ByteView rom)
{
    if (starts_with(flash_method, "sub_ecu_denso_sh7055"))
    {
        return {true, denso_sh7xxx(rom, 0x07FB80)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh7058_can_diesel"))
    {
        return {true, denso_sh705x_diesel(rom, 0x0FFB80)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh7058s_diesel_densocan"))
    {
        return {true, denso_sh705x_diesel(rom, 0x0FFB80)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh7058"))
    {
        return {true, denso_sh7xxx(rom, 0x0FFB80)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh72531_can"))
    {
        return {true, denso_sh7xxx(rom, 0x13F500)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_1n83m_4m_can"))
    {
        return {true, denso_sh7xxx(rom, 0x3E3E00, -0x8F9C000)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_1n83m_1_5m_can"))
    {
        return {true, denso_sh7xxx(rom, 0x183E00, -0x8F9C000)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh7059_can_diesel"))
    {
        return {true, denso_sh705x_diesel(rom, 0x17FB80)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh7059_diesel_densocan"))
    {
        return {true, denso_sh705x_diesel(rom, 0x17FB80)};
    }
    if (starts_with(flash_method, "sub_ecu_denso_sh72543_can_diesel"))
    {
        return {true, denso_sh705x_diesel(rom, 0x1FF800)};
    }
    if (starts_with(flash_method, "sub_tcu_denso_sh7055_can"))
    {
        return {true, ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(rom)};
    }
    if (starts_with(flash_method, "sub_tcu_denso_sh7058_can"))
    {
        return {true, denso_sh7xxx(rom, 0x0FFB80)};
    }
    if (starts_with(flash_method, "sub_ecu_hitachi_m32r_kline"))
    {
        if (starts_with(rom_id, "3"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(rom)};
        }
        if (starts_with(rom_id, "4") || starts_with(rom_id, "6"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
        }
        // flash_method matched, but RomId's leading digit is none of
        // "3"/"4"/"6" -- module available (no warning dialog), no family
        // runs, no bytes change. See Global Constraints.
        return {true, std::nullopt};
    }
    if (starts_with(flash_method, "sub_ecu_hitachi_m32r_can"))
    {
        return {true, ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
    }
    if (starts_with(flash_method, "sub_ecu_hitachi_sh7058_can"))
    {
        return {true, ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(rom)};
    }
    if (starts_with(flash_method, "sub_ecu_hitachi_sh72543r"))
    {
        return {true, ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(rom)};
    }
    if (starts_with(flash_method, "sub_tcu_hitachi_m32r_can"))
    {
        return {true, ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
    }
    if (starts_with(flash_method, "sub_tcu_hitachi_m32r_kline"))
    {
        return {true, ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
    }
    if (starts_with(flash_method, "sub_tcu_cvt_mitsu_mh8104_can"))
    {
        return {true, ChecksumTcuMitsuMH8104Can::calculate_checksum_result(rom)};
    }
    return {false, std::nullopt};
}

} // namespace

ChecksumCorrectionOutcome apply_checksum_correction(bytes::ByteView rom_data, const ChecksumSelection& selection)
{
    const flashdev_t *device = find_flash_device(selection.mcu_type);
    if (device == nullptr)
    {
        return {.status = ChecksumCorrectionOutcome::Status::UnknownMcuType};
    }
    if (selection.make != "Subaru" || selection.checksum_flag != "yes")
    {
        return {.status = ChecksumCorrectionOutcome::Status::NoModuleForProtocol};
    }
    if (rom_data.size() != device->romsize)
    {
        return {.status = ChecksumCorrectionOutcome::Status::BadRomSize};
    }

    const DispatchResult dispatch = dispatch_family(selection.flash_method, selection.rom_id, rom_data);
    if (!dispatch.module_available)
    {
        return {.status = ChecksumCorrectionOutcome::Status::NoModuleForProtocol};
    }
    return {.status = ChecksumCorrectionOutcome::Status::FamilyRan, .family_result = dispatch.result};
}

} // namespace fastecu::checksum
