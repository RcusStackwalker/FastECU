#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_types.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_types.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_types.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_types.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_types.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_types.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_types.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_types.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_types.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_types.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_types.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_types.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_types.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_types.h"
#include "src/backend/flash/flash_family.h"

namespace fastecu::flash
{

enum class FlashOperation
{
    kRead,
    kTestWrite,
    kWrite,
};

struct MemoryRegion
{
    std::uint32_t start;
    std::uint32_t length;

    bool operator==(const MemoryRegion&) const = default;
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
        kBeginEepromRead,
        kInspectEepromBytes,
        kCycleIgnition,
        // Plans carry these required confirmations before the desktop prompts.
        // The dialog collects operator answers before the synchronous,
        // dialog-free executor starts; declining prevents execution of the
        // bound attempt. Presence in the plan does not prove a grant.
        kEraseTrigger,
        kTopRegionBootstrap,
        // Requires confirmation that external programming voltage is applied
        // because the adapter cannot supply it, before the attempt runs.
        kApplyProgrammingVoltage,
        // Requires confirmation that VPP and MOD1 are connected for M32R
        // boot mode before the attempt runs.
        kApplyBootModeVoltages,
        // Hitachi SH7058 Read requires confirmation before opening the
        // adapter and starting the K-Line ROM read.
        kStartKlineRead,
        // Denso MC68HC16Y5 BDM Write requires confirmation before uploading
        // the kernel into RAM and starting it; the ROM is not written.
        kKernelBootstrap,
    };

    Id id;
    // Stable semantic arguments; desktop owns translated title/body/buttons.
    std::vector<std::pair<std::string, std::string>> arguments;

    bool operator==(const ConfirmationSpec&) const = default;
};

} // namespace fastecu::flash

template <> struct std::hash<fastecu::flash::ConfirmationSpec::Id>
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
using FamilyPlan =
    std::variant<DensoSh705xEepromKlinePlan, DensoSh705xEepromCanPlan, MitsuColtM32rCanPlan, SubaruMitsuM32rKlinePlan,
                 SubaruHitachiM32rKlinePlan, SubaruDensoMc68hc16y5_02Plan, SubaruDensoSh7055_02Plan,
                 SubaruHitachiM32rCanPlan, SubaruTcuCvtHitachiM32rCanPlan, SubaruTcuCvtMitsuMh8111CanPlan,
                 SubaruTcuCvtMitsuMh8104CanPlan, SubaruDenso1n83m_1_5mCanPlan, SubaruDensoSh72531CanPlan,
                 SubaruDensoSh72543CanDieselPlan, SubaruDenso1n83m_4mCanPlan, SubaruDensoSh705xDensoCanPlan,
                 SubaruTcuDensoSh705xCanPlan, SubaruDensoSh7058CanPlan, SubaruDensoSh7058CanDieselPlan,
                 SubaruTcuHitachiM32rKlinePlan, SubaruTcuHitachiM32rCanPlan, SubaruHitachiSh72543rCanPlan,
                 SubaruHitachiSh7058KlinePlan, SubaruHitachiSh7058CanPlan, SubaruUnisiaJecsPlan,
                 SubaruDensoSh705xKlinePlan, SubaruDensoMc68hc16y5_02BdmPlan, SubaruUnisiaJecsM32rKlinePlan,
                 SubaruUnisiaJecsM32rBootModeKernelPlan, SubaruUnisiaJecsM32rBootModeProgramPlan>;

// FamilyTraits and PlanValidator are specialized beside each plan type in its
// types header; kFamilyRequiresKernel is specialized below.

// Mitsu Colt CAN drives the ECU's own vendor bootloader and uploads only
// compile-time RAM helper routines, not a loaded kernel image.
template <> inline constexpr bool kFamilyRequiresKernel<MitsuColtM32rCanPlan> = false;

template <> inline constexpr bool kFamilyRequiresKernel<SubaruMitsuM32rKlinePlan> = false;

template <> inline constexpr bool kFamilyRequiresKernel<SubaruHitachiM32rKlinePlan> = false;

// Jumps to the ECU's resident on-board kernel via
// SecurityAccess + 0x10/0x42, uploading no image.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruHitachiM32rCanPlan> = false;

// Jumps to the TCU's resident on-board kernel via
// SecurityAccess + 0x10/0x02, uploading no image.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruTcuCvtHitachiM32rCanPlan> = false;

// Jumps to the TCU's resident on-board kernel via
// SecurityAccess + 0x10/0x42, uploading no image (no kernel-alive pre-check
// shortcut, unlike SubaruTcuCvtHitachiM32rCanPlan -- connect_bootloader
// always runs its full sequence for this family).
template <> inline constexpr bool kFamilyRequiresKernel<SubaruTcuCvtMitsuMh8111CanPlan> = false;

// Jumps to the TCU's resident on-board kernel via
// SecurityAccess + 0x10/0x42, uploading no image -- the same shape as
// SubaruTcuCvtMitsuMh8111CanPlan. Unlike MH8111, every response-content
// check in legacy after the kernel-alive probe is commented out
// (`// return STATUS_ERROR;`); this family tolerates any ECU response
// content and only a transport-level failure stops it.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruTcuCvtMitsuMh8104CanPlan> = false;

// Jumps to the ECU's resident on-board kernel via
// 0x10 0x42 (bench) or 0x10 0x62 (in-car), uploading no image.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDenso1n83m_1_5mCanPlan> = false;

// Same resident on-board kernel jump as its 1N83M
// sibling, via 0x10 0x42 (bench) or 0x10 0x62 (in-car); no image is uploaded.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDensoSh72531CanPlan> = false;

// Jumps to the ECU's resident on-board kernel via
// 0x10 0x42 (bench) or 0x10 0x62 (in-car), uploading no image. Diesel family,
// single-block flash geometry.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDensoSh72543CanDieselPlan> = false;

// The 4MB variant of the 1N83M family: same resident
// on-board kernel jump via 0x10 0x42 (bench) or 0x10 0x62 (in-car), no image
// uploaded.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDenso1n83m_4mCanPlan> = false;

// Authenticates against the TCU's resident bootloader
// via SecurityAccess and reads with 0xA0 block reads, uploading no image.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruTcuHitachiM32rKlinePlan> = false;

// Connects to the TCU's resident on-board kernel
// (connect_bootloader) and drives it with page-read/block-write commands
// over CAN; no image is uploaded.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruTcuHitachiM32rCanPlan> = false;
template <> inline constexpr bool kFamilyRequiresKernel<SubaruHitachiSh72543rCanPlan> = false;
template <> inline constexpr bool kFamilyRequiresKernel<SubaruHitachiSh7058KlinePlan> = false;
template <> inline constexpr bool kFamilyRequiresKernel<SubaruHitachiSh7058CanPlan> = false;
template <> inline constexpr bool kFamilyRequiresKernel<SubaruUnisiaJecsPlan> = false;

// Write uploads the cfg kernel over BDM, but carries
// it as the plan image (the bytes written to RAM), not as a KernelImage.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDensoMc68hc16y5_02BdmPlan> = false;

// The ECU's own boot ROM handles flash mode; no
// kernel is uploaded.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruUnisiaJecsM32rKlinePlan> = false;

// The kernel attempt carries the cfg kernel as its plan
// image, as 6c-1 BDM does: the _bootmode cfg entries declare no kernel_addr,
// and the M32R boot ROM places the kernel itself. The program attempt uploads
// nothing.
template <> inline constexpr bool kFamilyRequiresKernel<SubaruUnisiaJecsM32rBootModeKernelPlan> = false;
template <> inline constexpr bool kFamilyRequiresKernel<SubaruUnisiaJecsM32rBootModeProgramPlan> = false;

} // namespace fastecu::flash
