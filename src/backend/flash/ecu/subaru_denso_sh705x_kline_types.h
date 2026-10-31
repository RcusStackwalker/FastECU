#pragma once
#include <cstdint>

#include "src/backend/flash/flash_family.h"
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
};
template <> inline constexpr bool kFamilyRequiresKernel<SubaruDensoSh705xKlinePlan> = true;

template <> struct PlanValidator<SubaruDensoSh705xKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

} // namespace fastecu::flash
