#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
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
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<SubaruDensoSh7058CanDieselPlan>
{
    static Status Validate(const FlashPlan& plan);
};

inline constexpr DensoBeefCanProfile kSubaruDensoSh7058CanDieselBeefProfile{
    .device = "ECU", .family = "diesel", .pdu_format = BeefPduFormat::kDescribe};

Result<FlashPlan> BuildSubaruDensoSh7058CanDieselPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                      KernelImage kernel);
Status ValidateSubaruDensoSh7058CanDieselPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh7058CanDieselProtocols();
} // namespace fastecu::flash
