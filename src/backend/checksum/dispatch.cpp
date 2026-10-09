#include "src/backend/checksum/dispatch.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh705x_diesel.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh7xxx.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_can.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_kline.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh7058.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh72543r.h"
#include "src/algorithms/checksum/checksum_tcu_mitsu_mh8104_can.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_denso_sh7055.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_hitachi_m32r_can.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::checksum
{
namespace
{
constexpr std::uint32_t kDensoTableLength = 17 * 12;

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

ChecksumResult denso_sh7xxx(bytes::ByteView rom, std::uint32_t area_start, std::int32_t offset = 0)
{
    ChecksumResult result =
        ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(rom, area_start, kDensoTableLength, offset);
    if (result.Changed())
    {
        // This wrapper supplies the family title only for the Corrected path.
        // Other outcomes retain the algorithm-specific body text used by the
        // non-aggregated result dialog.
        result.message = "Subaru Denso SH705x Checksum";
    }
    return result;
}

ChecksumResult denso_sh705x_diesel(bytes::ByteView rom, std::uint32_t area_start)
{
    return ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(rom, area_start, kDensoTableLength);
}

struct DispatchResult
{
    bool module_available = false;
    std::optional<ChecksumResult> result;
};

enum class Route
{
    kDensoSh7xxx,
    kDensoDiesel,
    kDensoTcuSh7055,
    kM32rByRomId,
    kM32rCan,
    kSh7058,
    kSh72543r,
    kHitachiM32rTcu,
    kMitsuMh8104Tcu,
    kMitsuColtM32rCan,
};

struct RouteSpec
{
    std::string_view prefix;
    std::string_view make; // the selected vehicle's make (ConfigSession)
    Route route;
    std::uint32_t table_offset = 0;
    std::int32_t address_offset = 0;
};

// First prefix match wins, and only for the make the route belongs to, so a
// non-Subaru make cannot reach a Subaru family and vice versa. Keep specific
// overlapping routes before their general prefixes; notably sh7058_can_diesel
// must precede plain sh7058.
constexpr std::array kRoutes{
    RouteSpec{"sub_ecu_denso_sh7055", "Subaru", Route::kDensoSh7xxx, 0x07FB80},
    RouteSpec{"sub_ecu_denso_sh7058_can_diesel", "Subaru", Route::kDensoDiesel, 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh7058s_diesel_densocan", "Subaru", Route::kDensoDiesel, 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh7058", "Subaru", Route::kDensoSh7xxx, 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh72531_can", "Subaru", Route::kDensoSh7xxx, 0x13F500},
    RouteSpec{"sub_ecu_denso_1n83m_4m_can", "Subaru", Route::kDensoSh7xxx, 0x3E3E00, -0x8F9C000},
    RouteSpec{"sub_ecu_denso_1n83m_1_5m_can", "Subaru", Route::kDensoSh7xxx, 0x183E00, -0x8F9C000},
    RouteSpec{"sub_ecu_denso_sh7059_can_diesel", "Subaru", Route::kDensoDiesel, 0x17FB80},
    RouteSpec{"sub_ecu_denso_sh7059_diesel_densocan", "Subaru", Route::kDensoDiesel, 0x17FB80},
    RouteSpec{"sub_ecu_denso_sh72543_can_diesel", "Subaru", Route::kDensoDiesel, 0x1FF800},
    RouteSpec{"sub_tcu_denso_sh7055_can", "Subaru", Route::kDensoTcuSh7055},
    RouteSpec{"sub_tcu_denso_sh7058_can", "Subaru", Route::kDensoSh7xxx, 0x0FFB80},
    RouteSpec{"sub_ecu_hitachi_m32r_kline", "Subaru", Route::kM32rByRomId},
    RouteSpec{"sub_ecu_hitachi_m32r_can", "Subaru", Route::kM32rCan},
    RouteSpec{"sub_ecu_hitachi_sh7058_can", "Subaru", Route::kSh7058},
    RouteSpec{"sub_ecu_hitachi_sh72543r", "Subaru", Route::kSh72543r},
    RouteSpec{"sub_tcu_hitachi_m32r_can", "Subaru", Route::kHitachiM32rTcu},
    RouteSpec{"sub_tcu_hitachi_m32r_kline", "Subaru", Route::kHitachiM32rTcu},
    RouteSpec{"sub_tcu_cvt_mitsu_mh8104_can", "Subaru", Route::kMitsuMh8104Tcu},
    // Colt CZT Z37A (47110032): one prefix covers the plain and vendor_ext
    // protocols at both 384 KiB and 512 KiB.
    RouteSpec{"mitsu_ecu_m32r_can", "Mitsubishi", Route::kMitsuColtM32rCan},
};

DispatchResult execute(const RouteSpec& spec, std::string_view rom_id, bytes::ByteView rom)
{
    switch (spec.route)
    {
    case Route::kDensoSh7xxx:
        return {true, denso_sh7xxx(rom, spec.table_offset, spec.address_offset)};
    case Route::kDensoDiesel:
        return {true, denso_sh705x_diesel(rom, spec.table_offset)};
    case Route::kDensoTcuSh7055:
        return {true, ChecksumTcuSubaruDensoSH7055::CalculateChecksumResult(rom)};
    case Route::kM32rByRomId:
        if (starts_with(rom_id, "3"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rKline::CalculateChecksumResult(rom)};
        }
        if (starts_with(rom_id, "4") || starts_with(rom_id, "6"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(rom)};
        }
        // The protocol has a module, but unknown ROM-ID prefixes deliberately
        // run no family and produce no missing-module warning.
        return {true, std::nullopt};
    case Route::kM32rCan:
        return {true, ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(rom)};
    case Route::kSh7058:
        return {true, ChecksumEcuSubaruHitachiSH7058::CalculateChecksumResult(rom)};
    case Route::kSh72543r:
        return {true, ChecksumEcuSubaruHitachiSh72543r::CalculateChecksumResult(rom)};
    case Route::kHitachiM32rTcu:
        return {true, ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(rom)};
    case Route::kMitsuMh8104Tcu:
        return {true, ChecksumTcuMitsuMH8104Can::CalculateChecksumResult(rom)};
    case Route::kMitsuColtM32rCan:
        return {true, ChecksumEcuMitsuM32rCan::CalculateChecksumResult(rom)};
    }
    std::unreachable();
}

DispatchResult dispatch_family(std::string_view make, std::string_view flash_method, std::string_view rom_id,
                               bytes::ByteView rom)
{
    for (const RouteSpec& spec : kRoutes)
    {
        if (spec.make == make && starts_with(flash_method, spec.prefix))
        {
            return execute(spec, rom_id, rom);
        }
    }
    return {false, std::nullopt};
}
} // namespace

bool has_route(std::string_view make, std::string_view flash_method)
{
    return std::ranges::any_of(kRoutes, [&](const RouteSpec& spec)
                               { return spec.make == make && starts_with(flash_method, spec.prefix); });
}

ChecksumCorrectionOutcome apply_checksum_correction(bytes::ByteView rom_data, const ChecksumSelection& selection)
{
    const FlashDevice *device = fastecu::flash::find_flash_device(selection.mcu_type);
    if (device == nullptr)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kUnknownMcuType};
    }
    if (selection.checksum_flag != "yes")
    {
        return {.status = ChecksumCorrectionOutcome::Status::kNoModuleForProtocol};
    }
    if (rom_data.size() != device->romsize)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kBadRomSize};
    }

    const DispatchResult dispatch = dispatch_family(selection.make, selection.flash_method, selection.rom_id, rom_data);
    if (!dispatch.module_available)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kNoModuleForProtocol};
    }
    return {.status = ChecksumCorrectionOutcome::Status::kFamilyRan, .family_result = dispatch.result};
}

} // namespace fastecu::checksum
