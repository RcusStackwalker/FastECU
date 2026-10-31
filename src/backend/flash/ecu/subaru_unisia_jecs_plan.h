#pragma once

#include <span>
#include <string_view>

#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
struct SubaruUnisiaJecsPlan
{
    int initial_baud;
    bool even_parity;
};

template <> struct FamilyTraits<SubaruUnisiaJecsPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruUnisiaJecs;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruUnisiaJecsPlan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruUnisiaJecsPlan(FlashOperation operation, std::string_view protocol_name,
                                            std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruUnisiaJecsPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruUnisiaJecsProtocols();
} // namespace fastecu::flash
