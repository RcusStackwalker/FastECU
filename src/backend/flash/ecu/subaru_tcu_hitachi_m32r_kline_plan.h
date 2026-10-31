#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Legacy: flash_tcu_subaru_hitachi_m32r_kline_operation.{h,cpp}, read path
// only. Its execute() "test_write"/"write" branch logged "Not yet
// implemented" and then returned connect_bootloader()'s STATUS_SUCCESS,
// reporting a successful write that wrote nothing; this plan rejects both
// operations instead. See the original port plan's "Deliberate Divergences".
struct SubaruTcuHitachiM32rKlinePlan
{
    std::uint8_t tester_id;   // 0xf0
    std::uint8_t target_id;   // 0x18, not the ECU family's 0x10
    int baud;                 // 4800, never changed mid-session
    std::uint32_t block_size; // 96, not the ECU family's 128
};

// Requires no kernel: Authenticates against the TCU's resident bootloader
// via SecurityAccess and reads with 0xA0 block reads, uploading no image.
template <> struct FamilyTraits<SubaruTcuHitachiM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruTcuHitachiM32rKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruTcuHitachiM32rKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruTcuHitachiM32rKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruTcuHitachiM32rKlinePlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruTcuHitachiM32rKlineProtocols();
} // namespace fastecu::flash
