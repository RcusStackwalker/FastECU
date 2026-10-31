#pragma once

#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Framed SSM over plain K-Line; the ROM size comes
// from the plan's transfer region, so only the session parameters live here.
struct SubaruUnisiaJecsM32rKlinePlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;

    bool operator==(const SubaruUnisiaJecsM32rKlinePlan&) const = default;
};

template <> struct FamilyTraits<SubaruUnisiaJecsM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruUnisiaJecsM32rKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
};

template <> struct PlanValidator<SubaruUnisiaJecsM32rKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};
} // namespace fastecu::flash
