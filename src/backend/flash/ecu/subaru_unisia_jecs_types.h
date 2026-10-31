#pragma once

#include "src/backend/flash/flash_family.h"
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
};

template <> struct PlanValidator<SubaruUnisiaJecsPlan>
{
    static Status Validate(const FlashPlan& plan);
};
} // namespace fastecu::flash
