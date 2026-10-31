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

} // namespace fastecu::flash
