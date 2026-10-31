#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Wire and transport constants for the proprietary Subaru DensoCAN bootloader
// family. It begins and ends on 11-bit ISO-15765, with a 29-bit raw CAN
// bootloader transition only when a kernel upload is necessary.
struct SubaruDensoSh705xDensoCanPlan
{
    std::uint32_t iso_request_id{0x7E0};
    std::uint32_t iso_response_id{0x7E8};
    std::uint32_t raw_transmit_id{0x000FFFFE};
    std::uint32_t raw_receive_id{0x21};
    int bitrate{500000};
    bool iso_extended_id{false};
    bool raw_extended_id{true};
};

template <> struct FamilyTraits<SubaruDensoSh705xDensoCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh705xDensoCan;
    static constexpr TransportKind kTransport = TransportKind::kCanRawIso15765;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<SubaruDensoSh705xDensoCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruDensoSh705xDensocanPlan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                     KernelImage kernel);
Status ValidateSubaruDensoSh705xDensocanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh705xDensoCanProtocols();
} // namespace fastecu::flash
