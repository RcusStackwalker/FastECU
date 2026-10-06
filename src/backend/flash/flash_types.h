#pragma once
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
    // Serves all four mitsu_ecu_m32r_can capacity and
    // vendor-authorization variants; both properties are plan fields, not
    // separate families, matching the legacy class this replaces.
    MitsuColtM32rCan,
    SubaruMitsuM32rKline,
    SubaruHitachiM32rKline,
    SubaruDensoMc68hc16y5_02,
    SubaruDensoSh7055_02,
    SubaruHitachiM32rCan,
    SubaruTcuCvtHitachiM32rCan,
    SubaruTcuCvtMitsuMh8111Can,
    SubaruTcuCvtMitsuMh8104Can,
    // Denso ISO-15765 bootloader dialect.
    SubaruDenso1n83m_1_5mCan,
    SubaruDensoSh72531Can,
    SubaruDensoSh72543CanDiesel,
    SubaruDenso1n83m_4mCan,
    SubaruDensoSh705xDensoCan,
    SubaruTcuDensoSh705xCan,
    SubaruDensoSh7058Can,
    SubaruDensoSh7058CanDiesel,
    SubaruTcuHitachiM32rKline,
    SubaruTcuHitachiM32rCan,
    SubaruHitachiSh72543rCan,
    SubaruHitachiSh7058,
    SubaruUnisiaJecs,
    SubaruDensoSh705xKline,
    SubaruDensoMc68hc16y5_02Bdm,
    SubaruUnisiaJecsM32rKline,
    SubaruUnisiaJecsM32rBootModeKernel,
    SubaruUnisiaJecsM32rBootModeProgram,
};

enum class TransportKind
{
    Kline,
    CanIso15765,
    CanRawIso15765,
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
        BeginEepromRead,
        InspectEepromBytes,
        CycleIgnition,
        // Both are collected by the desktop dialog
        // BEFORE the executor starts: a synchronous, dialog-free executor
        // cannot block mid-run for a human answer. Presence in
        // FlashPlan::confirmations() therefore means "granted" -- an
        // operator who declines either one causes the dialog to never build
        // a plan at all.
        EraseTrigger,
        TopRegionBootstrap,
        // Same contract as the two above: the
        // operator confirmed, before the executor started, that external
        // programming voltage is applied because the adapter cannot supply
        // it.
        ApplyProgrammingVoltage,
        // Same contract: the operator confirmed, before
        // the executor started, that VPP and MOD1 are connected for M32R
        // boot mode.
        ApplyBootModeVoltages,
        // Same contract as the four above. Hitachi SH7058 Read: the operator
        // confirmed opening the adapter and starting the K-Line ROM read.
        StartKlineRead,
        // Same contract. Denso MC68HC16Y5 BDM Write: the operator confirmed
        // uploading the kernel into RAM and starting it; the ROM is not
        // written.
        KernelBootstrap,
    };

    Id id;
    // Stable semantic arguments; desktop owns translated title/body/buttons.
    std::vector<std::pair<std::string, std::string>> arguments;
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

// The FlashFamily tag and TransportKind each plan alternative belongs to.
//
// Deliberately has no primary definition: a new alternative added to
// FamilyPlan above must specialize this, right here, next to the variant it
// classifies, or every use fails to compile. That is stricter than
// kFamilyRequiresKernel below, which has a safe default (true) to fall
// back on -- there is no safe default family or transport to guess, and a
// wrong guess would let a plan reach the executor of a different family.
//
// validate_and_build consumes this instead of a hand-written switch over
// FlashFamily, so adding a family is a single-file edit here and cannot
// silently bind the wrong variant to a family.
template <typename PlanT> struct FamilyTraits;

template <> struct FamilyTraits<DensoSh705xEepromKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::DensoSh705xEepromKline;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<DensoSh705xEepromCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::DensoSh705xEepromCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<MitsuColtM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::MitsuColtM32rCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruMitsuM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruMitsuM32rKline;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruHitachiM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruHitachiM32rKline;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruDensoMc68hc16y5_02Plan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoMc68hc16y5_02;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruDensoSh7055_02Plan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh7055_02;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruHitachiM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruHitachiM32rCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruTcuCvtHitachiM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruTcuCvtHitachiM32rCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruTcuCvtMitsuMh8111CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruTcuCvtMitsuMh8111Can;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruTcuCvtMitsuMh8104CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruTcuCvtMitsuMh8104Can;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDenso1n83m_1_5mCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDenso1n83m_1_5mCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDensoSh72531CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh72531Can;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDensoSh72543CanDieselPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh72543CanDiesel;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDenso1n83m_4mCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDenso1n83m_4mCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDensoSh705xDensoCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh705xDensoCan;
    static constexpr TransportKind kTransport = TransportKind::CanRawIso15765;
};

template <> struct FamilyTraits<SubaruTcuDensoSh705xCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruTcuDensoSh705xCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDensoSh7058CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh7058Can;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruDensoSh7058CanDieselPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh7058CanDiesel;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruTcuHitachiM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruTcuHitachiM32rKline;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruTcuHitachiM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruTcuHitachiM32rCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruUnisiaJecsPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruUnisiaJecs;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruDensoSh705xKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoSh705xKline;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruDensoMc68hc16y5_02BdmPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruDensoMc68hc16y5_02Bdm;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruUnisiaJecsM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruUnisiaJecsM32rKline;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeKernelPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruUnisiaJecsM32rBootModeKernel;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeProgramPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruUnisiaJecsM32rBootModeProgram;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruHitachiSh72543rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruHitachiSh72543rCan;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

template <> struct FamilyTraits<SubaruHitachiSh7058KlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruHitachiSh7058;
    static constexpr TransportKind kTransport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruHitachiSh7058CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::SubaruHitachiSh7058;
    static constexpr TransportKind kTransport = TransportKind::CanIso15765;
};

// Whether validate_and_build requires FlashPlanFields::kernel to be set for
// this family's plan type. Defaults true (fail-closed): a family that skips
// the kernel must opt out explicitly, right here, next to the variant it
// classifies -- never by editing flash_validation.cpp.
template <typename PlanT> inline constexpr bool kFamilyRequiresKernel = true;

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
