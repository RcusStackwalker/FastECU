#pragma once

#include <cstdint>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

enum class SubaruDensoSh7058CanSecurity
{
    kStock,
    kEcuTek,
    kRaceRom,
    kRaceRomAlt,
    kCobb,
};

struct SubaruDensoSh7058CanPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
    SubaruDensoSh7058CanSecurity security{SubaruDensoSh7058CanSecurity::kStock};
};

template <> struct FamilyTraits<SubaruDensoSh7058CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh7058Can;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDensoSh7058CanPlan> = true;

template <> struct PlanValidator<SubaruDensoSh7058CanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

inline constexpr DensoBeefCanProfile kSubaruDensoSh7058CanBeefProfile{
    .device = "ECU", .family = "petrol", .pdu_format = BeefPduFormat::kHex};

} // namespace fastecu::flash
