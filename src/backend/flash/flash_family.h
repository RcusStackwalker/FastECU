#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>

namespace fastecu::flash
{

enum class FlashFamily
{
    kDensoSh705xEepromKline,
    kDensoSh705xEepromCan,
    // Serves all four mitsu_ecu_m32r_can capacity and
    // vendor-authorization variants; both properties are plan fields, not
    // separate families, matching the legacy class this replaces.
    kMitsuColtM32rCan,
    kSubaruMitsuM32rKline,
    kSubaruHitachiM32rKline,
    kSubaruDensoMc68hc16y502,
    kSubaruDensoSh705502,
    kSubaruHitachiM32rCan,
    kSubaruTcuCvtHitachiM32rCan,
    kSubaruTcuCvtMitsuMh8111Can,
    kSubaruTcuCvtMitsuMh8104Can,
    // Denso ISO-15765 bootloader dialect.
    kSubaruDenso1n83m15mCan,
    kSubaruDensoSh72531Can,
    kSubaruDensoSh72543CanDiesel,
    kSubaruDenso1n83m4mCan,
    kSubaruDensoSh705xDensoCan,
    kSubaruTcuDensoSh705xCan,
    kSubaruDensoSh7058Can,
    kSubaruDensoSh7058CanDiesel,
    kSubaruTcuHitachiM32rKline,
    kSubaruTcuHitachiM32rCan,
    kSubaruHitachiSh72543rCan,
    kSubaruHitachiSh7058,
    kSubaruUnisiaJecs,
    kSubaruDensoSh705xKline,
    kSubaruDensoMc68hc16y502Bdm,
    kSubaruUnisiaJecsM32rKline,
    kSubaruUnisiaJecsM32rBootModeKernel,
    kSubaruUnisiaJecsM32rBootModeProgram,
};

// Stable family identifiers, in FlashFamily order.
inline constexpr auto kFlashFamilyNames = std::to_array({
    std::pair{FlashFamily::kDensoSh705xEepromKline, std::string_view{"DensoSh705xEepromKline"}},
    std::pair{FlashFamily::kDensoSh705xEepromCan, std::string_view{"DensoSh705xEepromCan"}},
    std::pair{FlashFamily::kMitsuColtM32rCan, std::string_view{"MitsuColtM32rCan"}},
    std::pair{FlashFamily::kSubaruMitsuM32rKline, std::string_view{"SubaruMitsuM32rKline"}},
    std::pair{FlashFamily::kSubaruHitachiM32rKline, std::string_view{"SubaruHitachiM32rKline"}},
    std::pair{FlashFamily::kSubaruDensoMc68hc16y502, std::string_view{"SubaruDensoMc68hc16y5_02"}},
    std::pair{FlashFamily::kSubaruDensoSh705502, std::string_view{"SubaruDensoSh7055_02"}},
    std::pair{FlashFamily::kSubaruHitachiM32rCan, std::string_view{"SubaruHitachiM32rCan"}},
    std::pair{FlashFamily::kSubaruTcuCvtHitachiM32rCan, std::string_view{"SubaruTcuCvtHitachiM32rCan"}},
    std::pair{FlashFamily::kSubaruTcuCvtMitsuMh8111Can, std::string_view{"SubaruTcuCvtMitsuMh8111Can"}},
    std::pair{FlashFamily::kSubaruTcuCvtMitsuMh8104Can, std::string_view{"SubaruTcuCvtMitsuMh8104Can"}},
    std::pair{FlashFamily::kSubaruDenso1n83m15mCan, std::string_view{"SubaruDenso1n83m_1_5mCan"}},
    std::pair{FlashFamily::kSubaruDensoSh72531Can, std::string_view{"SubaruDensoSh72531Can"}},
    std::pair{FlashFamily::kSubaruDensoSh72543CanDiesel, std::string_view{"SubaruDensoSh72543CanDiesel"}},
    std::pair{FlashFamily::kSubaruDenso1n83m4mCan, std::string_view{"SubaruDenso1n83m_4mCan"}},
    std::pair{FlashFamily::kSubaruDensoSh705xDensoCan, std::string_view{"SubaruDensoSh705xDensoCan"}},
    std::pair{FlashFamily::kSubaruTcuDensoSh705xCan, std::string_view{"SubaruTcuDensoSh705xCan"}},
    std::pair{FlashFamily::kSubaruDensoSh7058Can, std::string_view{"SubaruDensoSh7058Can"}},
    std::pair{FlashFamily::kSubaruDensoSh7058CanDiesel, std::string_view{"SubaruDensoSh7058CanDiesel"}},
    std::pair{FlashFamily::kSubaruTcuHitachiM32rKline, std::string_view{"SubaruTcuHitachiM32rKline"}},
    std::pair{FlashFamily::kSubaruTcuHitachiM32rCan, std::string_view{"SubaruTcuHitachiM32rCan"}},
    std::pair{FlashFamily::kSubaruHitachiSh72543rCan, std::string_view{"SubaruHitachiSh72543rCan"}},
    std::pair{FlashFamily::kSubaruHitachiSh7058, std::string_view{"SubaruHitachiSh7058"}},
    std::pair{FlashFamily::kSubaruUnisiaJecs, std::string_view{"SubaruUnisiaJecs"}},
    std::pair{FlashFamily::kSubaruDensoSh705xKline, std::string_view{"SubaruDensoSh705xKline"}},
    std::pair{FlashFamily::kSubaruDensoMc68hc16y502Bdm, std::string_view{"SubaruDensoMc68hc16y5_02Bdm"}},
    std::pair{FlashFamily::kSubaruUnisiaJecsM32rKline, std::string_view{"SubaruUnisiaJecsM32rKline"}},
    std::pair{FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel, std::string_view{"SubaruUnisiaJecsM32rBootModeKernel"}},
    std::pair{FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram,
              std::string_view{"SubaruUnisiaJecsM32rBootModeProgram"}},
});

consteval bool FlashFamilyNamesFollowEnumOrder()
{
    const bool in_order =
        std::ranges::all_of(std::views::iota(std::size_t{0}, kFlashFamilyNames.size()), [](std::size_t index)
                            { return static_cast<std::size_t>(kFlashFamilyNames[index].first) == index; });
    return in_order &&
           static_cast<std::size_t>(FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram) + 1 == kFlashFamilyNames.size();
}
static_assert(FlashFamilyNamesFollowEnumOrder(), "name every FlashFamily, in declaration order");

constexpr std::string_view FlashFamilyName(FlashFamily family)
{
    const auto index = static_cast<std::size_t>(family);
    return index < kFlashFamilyNames.size() ? kFlashFamilyNames[index].second : "Unknown";
}

enum class TransportKind
{
    kKline,
    kCanIso15765,
    kCanRawIso15765,
};

// The facts about a plan type: the FlashFamily tag and TransportKind it
// belongs to, and whether ValidateAndBuild requires FlashPlanFields::kernel
// for it. A specialization provides
//   static constexpr FlashFamily kFamily;
//   static constexpr TransportKind kTransport;
//   static constexpr bool kRequiresKernel;
//
// Deliberately has no primary definition, and kRequiresKernel no default: a
// plan type must specialize this in its <f>_types.h and decide whether it
// uploads a kernel, or wrapping it in a FamilyPlan fails to compile. There is
// no safe default family or transport to guess, and a wrong guess would let a
// plan reach the executor of a different family.
//
// FamilyPlan records these when it wraps a plan, so ValidateAndBuild reads
// them without knowing every plan type (ADR 0030).
template <typename PlanT> struct FamilyTraits;

// A family's plan validator, reached by plan admission (FamilyExecutor in
// flash_executor.h). A specialization provides
// `static Status Validate(const FlashPlan&)`, forwarding to the family's
// Validate...Plan function.
//
// Like FamilyTraits, it deliberately has no primary definition: a family
// whose executor derives from FamilyExecutor must specialize it, declared in
// the plan type's `<f>_types.h` and defined in the family's plan `.cpp`, or
// the executor fails to compile.
template <typename PlanT> struct PlanValidator;

class FlashPlan;

} // namespace fastecu::flash
