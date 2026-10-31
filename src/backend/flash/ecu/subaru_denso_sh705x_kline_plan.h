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
// Legacy connect_bootloader() picks the ECUTEK seed-key transformation when the
// flash method (the protocol name) ends in "_ecutek".
enum class SubaruDensoSh705xKlineSeedKey
{
    kStock,
    kEcuTek,
};

struct SubaruDensoSh705xKlinePlan
{
    int initial_baud;       // 4800, execute()
    std::uint8_t tester_id; // 0xF0, execute()
    std::uint8_t target_id; // 0x10, execute()
    SubaruDensoSh705xKlineSeedKey seed_key;

    bool operator==(const SubaruDensoSh705xKlinePlan&) const = default;
};

template <> struct FamilyTraits<SubaruDensoSh705xKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh705xKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<SubaruDensoSh705xKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruDensoSh705xKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                  KernelImage kernel);
Status ValidateSubaruDensoSh705xKlinePlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh705xKlineProtocols();
} // namespace fastecu::flash
