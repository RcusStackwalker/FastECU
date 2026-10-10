#include "src/backend/checksum/dispatch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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
#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_error.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::checksum
{
namespace
{
constexpr std::uint32_t kDensoTableLength = 17 * 12;

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

ChecksumResult DensoSh7xxx(const memory::MemoryView& rom, std::uint32_t table_address)
{
    ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(
        rom, memory::FlashAddress{table_address}, kDensoTableLength);
    if (result.Changed())
    {
        // This wrapper supplies the family title only for the Corrected path.
        // Other outcomes retain the algorithm-specific body text used by the
        // non-aggregated result dialog.
        result.message = "Subaru Denso SH705x Checksum";
    }
    return result;
}

ChecksumResult DensoSh705xDiesel(const memory::MemoryView& rom, std::uint32_t table_address)
{
    return ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(rom, memory::FlashAddress{table_address},
                                                                       kDensoTableLength);
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
    std::uint32_t table_address = 0; // ECU address of the Denso checksum table
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
    RouteSpec{"sub_ecu_denso_1n83m_4m_can", "Subaru", Route::kDensoSh7xxx, 0x0937FE00},
    RouteSpec{"sub_ecu_denso_1n83m_1_5m_can", "Subaru", Route::kDensoSh7xxx, 0x0911FE00},
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

DispatchResult Execute(const RouteSpec& spec, std::string_view rom_id, const memory::MemoryView& view)
{
    switch (spec.route)
    {
    case Route::kDensoSh7xxx:
        return {true, DensoSh7xxx(view, spec.table_address)};
    case Route::kDensoDiesel:
        return {true, DensoSh705xDiesel(view, spec.table_address)};
    case Route::kDensoTcuSh7055:
        return {true, ChecksumTcuSubaruDensoSH7055::CalculateChecksumResult(view)};
    case Route::kM32rByRomId:
        if (StartsWith(rom_id, "3"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rKline::CalculateChecksumResult(view)};
        }
        if (StartsWith(rom_id, "4") || StartsWith(rom_id, "6"))
        {
            return {true, ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(view)};
        }
        // The protocol has a module, but unknown ROM-ID prefixes deliberately
        // run no family and produce no missing-module warning.
        return {true, std::nullopt};
    case Route::kM32rCan:
        return {true, ChecksumEcuSubaruHitachiM32rCan::CalculateChecksumResult(view)};
    case Route::kSh7058:
        return {true, ChecksumEcuSubaruHitachiSH7058::CalculateChecksumResult(view)};
    case Route::kSh72543r:
        return {true, ChecksumEcuSubaruHitachiSh72543r::CalculateChecksumResult(view)};
    case Route::kHitachiM32rTcu:
        return {true, ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(view)};
    case Route::kMitsuMh8104Tcu:
        return {true, ChecksumTcuMitsuMH8104Can::CalculateChecksumResult(view)};
    case Route::kMitsuColtM32rCan:
        return {true, ChecksumEcuMitsuM32rCan::CalculateChecksumResult(view)};
    }
    std::unreachable();
}

DispatchResult DispatchFamily(std::string_view make, std::string_view flash_method, std::string_view rom_id,
                              const memory::MemoryView& rom)
{
    for (const RouteSpec& spec : kRoutes)
    {
        if (spec.make == make && StartsWith(flash_method, spec.prefix))
        {
            return Execute(spec, rom_id, rom);
        }
    }
    return {false, std::nullopt};
}

// Writes each run of bytes `after` changed relative to `before` into `image`
// through its memory map. The first refused write is the error.
std::expected<void, memory::MemoryError> WriteBack(memory::MemoryImage& image, const memory::MemoryView& before,
                                                   bytes::ByteView after)
{
    const bytes::ByteView original = before.Data();
    std::size_t index = 0;
    while (index < original.size())
    {
        if (original[index] == after[index])
        {
            ++index;
            continue;
        }
        const std::size_t run_start = index;
        while (index < original.size() && original[index] != after[index])
        {
            ++index;
        }
        // The view's range holds every index, so the address is representable.
        const memory::FlashAddress address =
            *before.Range().Start().Advance(memory::ByteCount{static_cast<std::uint32_t>(run_start)});
        if (auto written = image.Write(address, after.subspan(run_start, index - run_start)); !written.has_value())
        {
            return written;
        }
    }
    return {};
}
} // namespace

bool HasRoute(std::string_view make, std::string_view flash_method)
{
    return std::ranges::any_of(kRoutes, [&](const RouteSpec& spec)
                               { return spec.make == make && StartsWith(flash_method, spec.prefix); });
}

ChecksumCorrectionOutcome ApplyChecksumCorrection(const memory::MemoryImage& image, const ChecksumSelection& selection)
{
    const FlashDevice *device = fastecu::flash::FindFlashDevice(selection.mcu_type);
    if (device == nullptr)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kUnknownMcuType};
    }
    if (selection.checksum_flag != "yes")
    {
        return {.status = ChecksumCorrectionOutcome::Status::kNoModuleForProtocol};
    }
    if (image.File().size() != device->romsize)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kBadRomSize};
    }
    // A map with addresses no block holds has no contiguous range for a family
    // to run on.
    const std::expected<memory::MemoryView, memory::MemoryError> view = image.Render(image.Map().Span());
    if (!view.has_value())
    {
        return {.status = ChecksumCorrectionOutcome::Status::kBadRomSize};
    }

    DispatchResult dispatch = DispatchFamily(selection.make, selection.flash_method, selection.rom_id, *view);
    if (!dispatch.module_available)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kNoModuleForProtocol};
    }
    ChecksumCorrectionOutcome outcome{.status = ChecksumCorrectionOutcome::Status::kFamilyRan,
                                      .family_result = std::move(dispatch.result)};
    // Every family returns the bytes of exactly the range it was given.
    if (!outcome.family_result.has_value() || !outcome.family_result->Ok() ||
        outcome.family_result->rom_data.size() != view->Data().size())
    {
        return outcome;
    }
    memory::MemoryImage corrected = image;
    if (auto written = WriteBack(corrected, *view, outcome.family_result->rom_data); !written.has_value())
    {
        outcome.family_result->status = ChecksumResult::Status::kUnsupportedRom;
        outcome.family_result->message = std::format(
            "Checksum correction would change ROM bytes the protocol cannot write: {}", written.error().detail);
        return outcome;
    }
    outcome.corrected_file.emplace(corrected.File().begin(), corrected.File().end());
    return outcome;
}

} // namespace fastecu::checksum
