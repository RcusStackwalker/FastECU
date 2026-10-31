#pragma once
#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

enum class HitachiM32rKlineSessionMode
{
    kNormal,
    kRecovery,
};

struct SubaruHitachiM32rKlinePlan
{
    HitachiM32rKlineSessionMode session_mode;
    std::uint8_t tester_id;
    std::uint8_t target_id;
    int initial_baud;
    int write_baud;
    int read_baud;
    std::uint32_t chunk_size;
    std::uint32_t read_address_bias;
};

template <> struct FamilyTraits<SubaruHitachiM32rKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruHitachiM32rKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruHitachiM32rKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

} // namespace fastecu::flash
