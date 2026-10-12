#include "src/backend/checksum/dispatch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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

// The route's window is the family's fixed layout, so the rendered bytes are
// exactly Family::kRomSize long.
template <typename Family> ChecksumResult RunFixedLayout(const memory::MemoryView& view)
{
    return Family::CalculateChecksumResult(typename Family::Rom(view.Data()));
}

static_assert(ChecksumEcuSubaruHitachiM32rKline::kRomSize == ChecksumEcuSubaruHitachiM32rCan::kRomSize,
              "the ROM-ID route renders one window for both M32R ECU families");

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

// A checksum family's ECU address range, as compile-time route data.
consteval memory::AddressRange<memory::FlashSpace> EcuWindow(std::uint32_t start, std::uint32_t size)
{
    const auto window =
        memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size});
    return window.has_value() ? *window
                              : throw std::invalid_argument("checksum window is empty or runs past the address space");
}

struct RouteSpec
{
    std::string_view prefix;
    std::string_view make; // the selected vehicle's make (ConfigSession)
    Route route;
    // The ECU addresses the family checksums. The ROM's memory map must span
    // exactly this range.
    memory::AddressRange<memory::FlashSpace> window;
    std::uint32_t table_address = 0; // ECU address of the Denso checksum table
};

// First prefix match wins, and only for the make the route belongs to, so a
// non-Subaru make cannot reach a Subaru family and vice versa. Keep specific
// overlapping routes before their general prefixes; notably sh7058_can_diesel
// must precede plain sh7058.
constexpr std::array kRoutes{
    RouteSpec{"sub_ecu_denso_sh7055", "Subaru", Route::kDensoSh7xxx, EcuWindow(0, 0x80000), 0x07FB80},
    RouteSpec{"sub_ecu_denso_sh7058_can_diesel", "Subaru", Route::kDensoDiesel, EcuWindow(0, 0x100000), 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh7058s_diesel_densocan", "Subaru", Route::kDensoDiesel, EcuWindow(0, 0x100000), 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh7058", "Subaru", Route::kDensoSh7xxx, EcuWindow(0, 0x100000), 0x0FFB80},
    RouteSpec{"sub_ecu_denso_sh72531_can", "Subaru", Route::kDensoSh7xxx, EcuWindow(0, 0x140000), 0x13F500},
    RouteSpec{"sub_ecu_denso_1n83m_4m_can", "Subaru", Route::kDensoSh7xxx, EcuWindow(0x08F9C000, 0x3E4000), 0x0937FE00},
    RouteSpec{"sub_ecu_denso_1n83m_1_5m_can", "Subaru", Route::kDensoSh7xxx, EcuWindow(0x08F9C000, 0x184000),
              0x0911FE00},
    RouteSpec{"sub_ecu_denso_sh7059_can_diesel", "Subaru", Route::kDensoDiesel, EcuWindow(0, 0x180000), 0x17FB80},
    RouteSpec{"sub_ecu_denso_sh7059_diesel_densocan", "Subaru", Route::kDensoDiesel, EcuWindow(0, 0x180000), 0x17FB80},
    RouteSpec{"sub_ecu_denso_sh72543_can_diesel", "Subaru", Route::kDensoDiesel, EcuWindow(0, 0x200000), 0x1FF800},
    RouteSpec{"sub_tcu_denso_sh7055_can", "Subaru", Route::kDensoTcuSh7055,
              EcuWindow(0, ChecksumTcuSubaruDensoSH7055::kRomSize)},
    RouteSpec{"sub_tcu_denso_sh7058_can", "Subaru", Route::kDensoSh7xxx, EcuWindow(0, 0x100000), 0x0FFB80},
    RouteSpec{"sub_ecu_hitachi_m32r_kline", "Subaru", Route::kM32rByRomId,
              EcuWindow(0, ChecksumEcuSubaruHitachiM32rKline::kRomSize)},
    RouteSpec{"sub_ecu_hitachi_m32r_can", "Subaru", Route::kM32rCan,
              EcuWindow(0, ChecksumEcuSubaruHitachiM32rCan::kRomSize)},
    RouteSpec{"sub_ecu_hitachi_sh7058_can", "Subaru", Route::kSh7058,
              EcuWindow(0, ChecksumEcuSubaruHitachiSH7058::kRomSize)},
    RouteSpec{"sub_ecu_hitachi_sh72543r", "Subaru", Route::kSh72543r,
              EcuWindow(0, ChecksumEcuSubaruHitachiSh72543r::kRomSize)},
    RouteSpec{"sub_tcu_hitachi_m32r_can", "Subaru", Route::kHitachiM32rTcu, EcuWindow(0, 0x80000)},
    RouteSpec{"sub_tcu_hitachi_m32r_kline", "Subaru", Route::kHitachiM32rTcu, EcuWindow(0, 0x80000)},
    RouteSpec{"sub_tcu_cvt_mitsu_mh8104_can", "Subaru", Route::kMitsuMh8104Tcu,
              EcuWindow(0, ChecksumTcuMitsuMH8104Can::kRomSize)},
    // Colt CZT Z37A (47110032). The 512 KiB protocols name their size as a
    // suffix, so they precede the 384 KiB prefix that covers plain and
    // vendor_ext.
    RouteSpec{"mitsu_ecu_m32r_can_512kb", "Mitsubishi", Route::kMitsuColtM32rCan, EcuWindow(0, 0x80000)},
    RouteSpec{"mitsu_ecu_m32r_can_vendor_ext_512kb", "Mitsubishi", Route::kMitsuColtM32rCan, EcuWindow(0, 0x80000)},
    RouteSpec{"mitsu_ecu_m32r_can", "Mitsubishi", Route::kMitsuColtM32rCan, EcuWindow(0, 0x60000)},
};

// The ROM size a fixed-layout route's family takes, or nullopt for a route
// that takes any window.
consteval std::optional<std::uint32_t> FixedRomSize(Route route)
{
    switch (route)
    {
    case Route::kDensoTcuSh7055:
        return ChecksumTcuSubaruDensoSH7055::kRomSize;
    case Route::kM32rByRomId:
        return ChecksumEcuSubaruHitachiM32rKline::kRomSize;
    case Route::kM32rCan:
        return ChecksumEcuSubaruHitachiM32rCan::kRomSize;
    case Route::kSh7058:
        return ChecksumEcuSubaruHitachiSH7058::kRomSize;
    case Route::kSh72543r:
        return ChecksumEcuSubaruHitachiSh72543r::kRomSize;
    case Route::kMitsuMh8104Tcu:
        return ChecksumTcuMitsuMH8104Can::kRomSize;
    case Route::kDensoSh7xxx:
    case Route::kDensoDiesel:
    case Route::kHitachiM32rTcu:
    case Route::kMitsuColtM32rCan:
        return std::nullopt;
    }
    return std::nullopt;
}

// What RunFixedLayout and the families assume of kRoutes: a fixed-layout
// window is [0, kRomSize), the Colt and Hitachi TCU families index from 0, a
// Denso route's primary checksum table lies inside its window (the diesel
// family's fixed second table is range-checked when it runs), and no route sits
// behind an earlier, more general prefix of its make, where it could never
// match.
consteval bool RoutesAreConsistent(std::span<const RouteSpec> routes)
{
    for (std::size_t index = 0; index < routes.size(); ++index)
    {
        const RouteSpec& route = routes[index];
        const std::optional<std::uint32_t> fixed = FixedRomSize(route.route);
        if (fixed.has_value() && (route.window.Start().Value() != 0 || route.window.Size().Value() != *fixed))
        {
            return false;
        }
        if ((route.route == Route::kMitsuColtM32rCan || route.route == Route::kHitachiM32rTcu) &&
            route.window.Start().Value() != 0)
        {
            return false;
        }
        const std::optional<memory::AddressRange<memory::FlashSpace>> table =
            memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{route.table_address},
                                                           memory::ByteCount{kDensoTableLength});
        if ((route.route == Route::kDensoSh7xxx || route.route == Route::kDensoDiesel) &&
            (!table.has_value() || !route.window.Contains(*table)))
        {
            return false;
        }
        for (const RouteSpec& earlier : routes.first(index))
        {
            if (earlier.make == route.make && route.prefix.starts_with(earlier.prefix))
            {
                return false;
            }
        }
    }
    return true;
}
static_assert(RoutesAreConsistent(kRoutes), "a checksum route breaks a route-table invariant");

std::optional<ChecksumResult> Execute(const RouteSpec& spec, std::string_view rom_id, const memory::MemoryView& view)
{
    switch (spec.route)
    {
    case Route::kDensoSh7xxx:
        return DensoSh7xxx(view, spec.table_address);
    case Route::kDensoDiesel:
        return DensoSh705xDiesel(view, spec.table_address);
    case Route::kDensoTcuSh7055:
        return RunFixedLayout<ChecksumTcuSubaruDensoSH7055>(view);
    case Route::kM32rByRomId:
        if (StartsWith(rom_id, "3"))
        {
            return RunFixedLayout<ChecksumEcuSubaruHitachiM32rKline>(view);
        }
        if (StartsWith(rom_id, "4") || StartsWith(rom_id, "6"))
        {
            return RunFixedLayout<ChecksumEcuSubaruHitachiM32rCan>(view);
        }
        // The protocol has a module, but unknown ROM-ID prefixes deliberately
        // run no family and produce no missing-module warning.
        return std::nullopt;
    case Route::kM32rCan:
        return RunFixedLayout<ChecksumEcuSubaruHitachiM32rCan>(view);
    case Route::kSh7058:
        return RunFixedLayout<ChecksumEcuSubaruHitachiSH7058>(view);
    case Route::kSh72543r:
        return RunFixedLayout<ChecksumEcuSubaruHitachiSh72543r>(view);
    case Route::kHitachiM32rTcu:
        return ChecksumTcuSubaruHitachiM32rCan::CalculateChecksumResult(view.Data());
    case Route::kMitsuMh8104Tcu:
        return RunFixedLayout<ChecksumTcuMitsuMH8104Can>(view);
    case Route::kMitsuColtM32rCan:
        return ChecksumEcuMitsuM32rCan::CalculateChecksumResult(view.Data());
    }
    std::unreachable();
}

const RouteSpec *FindRoute(std::string_view make, std::string_view flash_method)
{
    const auto found = std::ranges::find_if(kRoutes, [&](const RouteSpec& spec)
                                            { return spec.make == make && StartsWith(flash_method, spec.prefix); });
    return found == kRoutes.end() ? nullptr : std::to_address(found);
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
    return FindRoute(make, flash_method) != nullptr;
}

ChecksumCorrectionOutcome ApplyChecksumCorrection(const memory::MemoryImage& image, const ChecksumSelection& selection)
{
    if (fastecu::flash::FindFlashDevice(selection.mcu_type) == nullptr)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kUnknownMcuType};
    }
    if (selection.checksum_flag != "yes")
    {
        return {.status = ChecksumCorrectionOutcome::Status::kNoModuleForProtocol};
    }
    const RouteSpec *route = FindRoute(selection.make, selection.flash_method);
    if (route == nullptr)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kNoModuleForProtocol};
    }
    // The family's constants are addresses in the route's window, so the ROM
    // must cover exactly that range, with a block holding every address.
    if (image.Map().Span() != route->window)
    {
        return {.status = ChecksumCorrectionOutcome::Status::kBadRomSize};
    }
    const std::expected<memory::MemoryView, memory::MemoryError> view = image.Render(route->window);
    if (!view.has_value())
    {
        return {.status = ChecksumCorrectionOutcome::Status::kBadRomSize};
    }

    ChecksumCorrectionOutcome outcome{.status = ChecksumCorrectionOutcome::Status::kFamilyRan,
                                      .family_result = Execute(*route, selection.rom_id, *view)};
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
