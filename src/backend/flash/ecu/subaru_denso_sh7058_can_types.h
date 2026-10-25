#pragma once

#include <cstdint>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"

namespace fastecu::flash
{

enum class SubaruDensoSh7058CanSecurity
{
    kStock,
    kEcuTek,
    kRaceRom,
    kRaceRomAlt,
    kCobb,
};

struct SubaruDensoSh7058CanPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
    SubaruDensoSh7058CanSecurity security{SubaruDensoSh7058CanSecurity::kStock};
};

inline constexpr DensoBeefCanProfile kSubaruDensoSh7058CanBeefProfile{
    .device = "ECU", .family = "petrol", .pdu_format = BeefPduFormat::kHex};

} // namespace fastecu::flash
