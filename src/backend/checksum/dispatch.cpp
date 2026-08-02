#include "src/backend/checksum/dispatch.h"

#include <array>
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
constexpr std::uint32_t kDensoTableLength = 17 * 12;

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

ChecksumResult densoSh7xxx(bytes::ByteView rom, std::uint32_t area_start,
                           std::int32_t offset = 0)
{
    ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(
        rom, area_start, kDensoTableLength, offset);
    if (result.changed())
    {
        result.message = "Subaru Denso SH705x Checksum";
    }
    return result;
}

ChecksumResult densoDiesel(bytes::ByteView rom, std::uint32_t area_start)
{
    return ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(
        rom, area_start, kDensoTableLength);
}

struct DispatchResult
{
    bool module_available = false;
    std::optional<ChecksumResult> result;
};

enum class Route
{
    DensoSh7xxx,
    DensoDiesel,
    DensoTcuSh7055,
    M32rByRomId,
    M32rCan,
    Sh7058,
    Sh72543r,
    HitachiM32rTcu,
    MitsuMh8104Tcu,
};

struct RouteSpec
{
    std::string_view prefix;
    Route route;
    std::uint32_t table_offset = 0;
    std::int32_t address_offset = 0;
};

constexpr std::array kRoutes{
    RouteSpec{"sub_ecu_denso_sh7055", Route::DensoSh7xxx, 0x07FB80},
    RouteSpec{"sub_ecu_denso_sh7058_can_diesel", Route::DensoDiesel, 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh7058s_diesel_densocan", Route::DensoDiesel, 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh7058", Route::DensoSh7xxx, 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh72531_can", Route::DensoSh7xxx, 0x13F500},
    RouteSpec{"sub_ecu_denso_1n83m_4m_can", Route::DensoSh7xxx, 0x3E3E00, -0x8F9C000},
    RouteSpec{"sub_ecu_denso_1n83m_1_5m_can", Route::DensoSh7xxx, 0x183E00, -0x8F9C000},
    RouteSpec{"sub_ecu_denso_sh7059_can_diesel", Route::DensoDiesel, 0x17FB80},
    RouteSpec{"sub_ecu_denso_sh7059_diesel_densocan", Route::DensoDiesel, 0x17FB80},
    RouteSpec{"sub_ecu_denso_sh72543_can_diesel", Route::DensoDiesel, 0x1FF800},
    RouteSpec{"sub_tcu_denso_sh7055_can", Route::DensoTcuSh7055},
    RouteSpec{"sub_tcu_denso_sh7058_can", Route::DensoSh7xxx, 0x0FFB80},
    RouteSpec{"sub_ecu_hitachi_m32r_kline", Route::M32rByRomId},
    RouteSpec{"sub_ecu_hitachi_m32r_can", Route::M32rCan},
    RouteSpec{"sub_ecu_hitachi_sh7058_can", Route::Sh7058},
    RouteSpec{"sub_ecu_hitachi_sh72543r", Route::Sh72543r},
    RouteSpec{"sub_tcu_hitachi_m32r_can", Route::HitachiM32rTcu},
    RouteSpec{"sub_tcu_hitachi_m32r_kline", Route::HitachiM32rTcu},
    RouteSpec{"sub_tcu_cvt_mitsu_mh8104_can", Route::MitsuMh8104Tcu},
};

DispatchResult execute(const RouteSpec& spec, std::string_view rom_id,
                       bytes::ByteView rom)
{
    switch (spec.route)
    {
    case Route::DensoSh7xxx:
        return {true, densoSh7xxx(rom, spec.table_offset, spec.address_offset)};
    case Route::DensoDiesel:
        return {true, densoDiesel(rom, spec.table_offset)};
    case Route::DensoTcuSh7055:
        return {true, ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(rom)};
    case Route::M32rByRomId:
        if (startsWith(rom_id, "3"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(rom)};
        }
        if (startsWith(rom_id, "4") || startsWith(rom_id, "6"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
        }
        return {true, std::nullopt};
    case Route::M32rCan:
        return {true, ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
    case Route::Sh7058:
        return {true, ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(rom)};
    case Route::Sh72543r:
        return {true, ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(rom)};
    case Route::HitachiM32rTcu:
        return {true, ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(rom)};
    case Route::MitsuMh8104Tcu:
        return {true, ChecksumTcuMitsuMH8104Can::calculate_checksum_result(rom)};
    }
}

DispatchResult dispatchFamily(std::string_view flash_method,
                              std::string_view rom_id, bytes::ByteView rom)
{
    for (const RouteSpec& spec : kRoutes)
    {
        if (startsWith(flash_method, spec.prefix))
        {
            return execute(spec, rom_id, rom);
        }
    }
    return {false, std::nullopt};
}
} // namespace

ChecksumCorrectionOutcome apply_checksum_correction(bytes::ByteView rom_data,
                                                    const ChecksumSelection& selection)
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

    const DispatchResult dispatch = dispatchFamily(selection.flash_method, selection.rom_id, rom_data);
    if (!dispatch.module_available)
    {
        return {.status = ChecksumCorrectionOutcome::Status::NoModuleForProtocol};
    }
    return {.status = ChecksumCorrectionOutcome::Status::FamilyRan,
            .family_result = dispatch.result};
}

} // namespace fastecu::checksum
