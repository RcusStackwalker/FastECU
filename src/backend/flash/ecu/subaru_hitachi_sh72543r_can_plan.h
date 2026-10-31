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
// Legacy SH72543R CAN: 2 MiB read; only [0x6000, 0x200000) is programmed.
// TestWrite is rejected: legacy reflash_block ignores its test_write_arg.
struct SubaruHitachiSh72543rCanPlan
{
    std::uint32_t request_id;
    std::uint32_t response_id;
    int bitrate;
    bool extended_id;
    std::uint32_t page_size;
    std::uint32_t write_frame_size;
};

template <> struct FamilyTraits<SubaruHitachiSh72543rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruHitachiSh72543rCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruHitachiSh72543rCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruHitachiSh72543rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                    std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruHitachiSh72543rCanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruHitachiSh72543rCanProtocols();
} // namespace fastecu::flash
