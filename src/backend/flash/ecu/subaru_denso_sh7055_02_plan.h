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
// SH7055_02. Unlike MC68, tester_id/target_id ARE live -- the one
// surviving SSM-framed exchange (SID 0xBF ECU-ID read, read-only, Read
// operation only) uses them via SsmProtocol::addHeader().
struct SubaruDensoSh7055_02Plan
{
    std::uint8_t tester_id; // 0xf0
    std::uint8_t target_id; // 0x10
    bool read_ecu_id;       // true iff FlashOperation::kRead

    bool operator==(const SubaruDensoSh7055_02Plan&) const = default;
};

template <> struct FamilyTraits<SubaruDensoSh7055_02Plan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh705502;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<SubaruDensoSh7055_02Plan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruDensoSh705502Plan(FlashOperation operation, std::string_view protocol_name,
                                               std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                               KernelImage kernel);
Status ValidateSubaruDensoSh705502Plan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh705502Protocols();
} // namespace fastecu::flash
