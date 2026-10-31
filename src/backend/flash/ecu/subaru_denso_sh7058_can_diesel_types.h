#pragma once

#include <cstdint>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

struct SubaruDensoSh7058CanDieselPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
};

template <> struct FamilyTraits<SubaruDensoSh7058CanDieselPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh7058CanDiesel;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDensoSh7058CanDieselPlan> = true;

template <> struct PlanValidator<SubaruDensoSh7058CanDieselPlan>
{
    static Status Validate(const FlashPlan& plan);
};

inline constexpr DensoBeefCanProfile kSubaruDensoSh7058CanDieselBeefProfile{
    .device = "ECU", .family = "diesel", .pdu_format = BeefPduFormat::kDescribe};

} // namespace fastecu::flash
