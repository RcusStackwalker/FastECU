#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_types.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_types.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_types.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_types.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_types.h"

namespace fastecu::flash
{

enum class FlashOperation
{
    Read,
    TestWrite,
    Write,
};

enum class FlashFamily
{
    DensoSh705xEepromKline,
    DensoSh705xEepromCan,
    // Step 5 tail, wave 0. Serves all four mitsu_ecu_m32r_can capacity and
    // vendor-authorization variants; both properties are plan fields, not
    // separate families, matching the legacy class this replaces.
    MitsuColtM32rCan,
    SubaruMitsuM32rKline,
    SubaruHitachiM32rKline,
    // Step 5 tail, wave 2.
    SubaruDensoMc68hc16y5_02,
    SubaruDensoSh7055_02,
    // Step 5 tail, wave 3.
    SubaruHitachiM32rCan,
    SubaruTcuCvtHitachiM32rCan,
};

enum class TransportKind
{
    Kline,
    CanIso15765,
};

struct MemoryRegion
{
    std::uint32_t start;
    std::uint32_t length;
};

struct KernelImage
{
    std::string id; // diagnostic identity, not a filesystem path
    std::uint32_t load_address;
    bytes::Bytes bytes; // immutable snapshot owned by the plan
};

struct ConfirmationSpec
{
    enum class Id
    {
        BeginEepromRead,
        InspectEepromBytes,
        CycleIgnition,
        // Step 5 tail, wave 0. Both are collected by the desktop dialog
        // BEFORE the executor starts: a synchronous, dialog-free executor
        // cannot block mid-run for a human answer. Presence in
        // FlashPlan::confirmations() therefore means "granted" -- an
        // operator who declines either one causes the dialog to never build
        // a plan at all.
        EraseTrigger,
        TopRegionBootstrap,
    };

    Id id;
    // Stable semantic arguments; desktop owns translated title/body/buttons.
    std::vector<std::pair<std::string, std::string>> arguments;
};

} // namespace fastecu::flash

template <>
struct std::hash<fastecu::flash::ConfirmationSpec::Id>
{
    std::size_t operator()(fastecu::flash::ConfirmationSpec::Id id) const noexcept
    {
        return std::hash<int>{}(static_cast<int>(id));
    }
};

namespace fastecu::flash
{

// Each alternative's struct lives in a per-family (or, once a cluster is
// factored, per-cluster) types header included above -- see e.g.
// ecu/mitsu_colt_m32r_can_types.h and eeprom/denso_sh705x_eeprom_types.h.
// This file stays the single place that assembles the variant and classifies
// its alternatives; it does not own any individual family's fields.
using FamilyPlan = std::variant<
    DensoSh705xEepromKlinePlan,
    DensoSh705xEepromCanPlan,
    MitsuColtM32rCanPlan,
    SubaruMitsuM32rKlinePlan,
    SubaruHitachiM32rKlinePlan,
    SubaruDensoMc68hc16y5_02Plan,
    SubaruDensoSh7055_02Plan,
    SubaruHitachiM32rCanPlan,
    SubaruTcuCvtHitachiM32rCanPlan>;

// Whether validate_and_build requires FlashPlanFields::kernel to be set for
// this family's plan type. Defaults true (fail-closed): a family that skips
// the kernel must opt out explicitly, right here, next to the variant it
// classifies -- never by editing flash_validation.cpp.
template <typename PlanT>
inline constexpr bool family_requires_kernel_v = true;

// Mitsu Colt CAN drives the ECU's own vendor bootloader and uploads only
// compile-time RAM helper routines, not a loaded kernel image.
template <>
inline constexpr bool family_requires_kernel_v<MitsuColtM32rCanPlan> = false;

template <>
inline constexpr bool family_requires_kernel_v<SubaruMitsuM32rKlinePlan> = false;

template <>
inline constexpr bool family_requires_kernel_v<SubaruHitachiM32rKlinePlan> = false;

// Step 5 tail, wave 3. Jumps to the ECU's resident on-board kernel via
// SecurityAccess + 0x10/0x42, uploading no image.
template <>
inline constexpr bool family_requires_kernel_v<SubaruHitachiM32rCanPlan> = false;

// Step 5 tail, wave 3. Jumps to the TCU's resident on-board kernel via
// SecurityAccess + 0x10/0x02, uploading no image.
template <>
inline constexpr bool family_requires_kernel_v<SubaruTcuCvtHitachiM32rCanPlan> = false;

} // namespace fastecu::flash
