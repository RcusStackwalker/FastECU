#pragma once
#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

enum class DensoSecurityVariant
{
    kStock,
    kEcuTek,
    kCobb,
    kEcuTekRaceRom,
};

enum class EepromReadMode : std::uint8_t
{
    kMode2 = 2,
    kMode3 = 3,
    kMode4 = 4,
};

struct DensoSh705xEepromKlinePlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint8_t tester_id; // 0xf0 for the current family
    std::uint8_t target_id; // 0x10 for the current family; unrelated to
                            // FlashPlan::target_id(), which names the
                            // selected protocol/configuration string, not
                            // this numeric M32R/SH705x bus address.
    int initial_baud;       // 4800
    int kernel_baud;        // snapshotted resolved family value
};

template <> struct FamilyTraits<DensoSh705xEepromKlinePlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kDensoSh705xEepromKline;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<DensoSh705xEepromKlinePlan>
{
    static Status Validate(const FlashPlan& plan);
};

struct DensoSh705xEepromCanPlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false
};

template <> struct FamilyTraits<DensoSh705xEepromCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kDensoSh705xEepromCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<DensoSh705xEepromCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

} // namespace fastecu::flash
