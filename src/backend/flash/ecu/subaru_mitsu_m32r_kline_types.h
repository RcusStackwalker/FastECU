#pragma once
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

struct SubaruMitsuM32rKlinePlan
{
    std::uint8_t tester_id;
    std::uint8_t target_id;
    int initial_baud;
    int flash_baud;
    std::uint32_t chunk_size;
    bytes::Byte unread_prefix_fill;
};

template <> struct FamilyTraits<SubaruMitsuM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruMitsuM32rKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruMitsuM32rKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

} // namespace fastecu::flash
