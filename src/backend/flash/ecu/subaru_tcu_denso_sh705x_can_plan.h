#pragma once

#include <chrono>
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
// Wire setup for the ISO-15765-only Subaru Denso SH705x TCU family.
struct SubaruTcuDensoSh705xCanPlan
{
    std::uint32_t request_id{0x7E1};
    std::uint32_t response_id{0x7E9};
    int bitrate{500000};
    bool extended_id{false};
};

template <> struct FamilyTraits<SubaruTcuDensoSh705xCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruTcuDensoSh705xCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<SubaruTcuDensoSh705xCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

inline constexpr DensoBeefCanProfile kSubaruTcuDensoSh705xCanBeefProfile{.device = "TCU",
                                                                         .family = "TCU",
                                                                         .pdu_format = BeefPduFormat::kHex,
                                                                         .init_timeout =
                                                                             std::chrono::milliseconds{2000},
                                                                         .upper_hex_sizes = true};

Result<FlashPlan> BuildSubaruTcuDensoSh705xCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                   KernelImage kernel);
Status ValidateSubaruTcuDensoSh705xCanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruTcuDensoSh705xCanProtocols();
} // namespace fastecu::flash
