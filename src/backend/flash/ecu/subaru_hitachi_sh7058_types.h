#pragma once

#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
struct SubaruHitachiSh7058KlinePlan
{
    int initial_baud = 4800;
    std::uint8_t tester_id = 0xf0;
    std::uint8_t target_id = 0x10;
    std::uint32_t page_size = 0x80;

    bool operator==(const SubaruHitachiSh7058KlinePlan&) const = default;
};

template <> struct FamilyTraits<SubaruHitachiSh7058KlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruHitachiSh7058;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

// The two transports share one plan validator and one builder. Each executor
// supports one operation, so the mode guard sits in its own specialization and
// the shared validator keeps accepting both.
template <> struct PlanValidator<SubaruHitachiSh7058KlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

struct SubaruHitachiSh7058CanPlan
{
    int bitrate = 500000;
    std::uint32_t request_id = 0x7e0;
    std::uint32_t response_id = 0x7e8;
    bool extended_id = false;
    std::uint32_t frame_size = 0x100;

    bool operator==(const SubaruHitachiSh7058CanPlan&) const = default;
};

template <> struct FamilyTraits<SubaruHitachiSh7058CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruHitachiSh7058;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruHitachiSh7058CanPlan>
{
    static Status Validate(const FlashPlan& plan);
};
} // namespace fastecu::flash
