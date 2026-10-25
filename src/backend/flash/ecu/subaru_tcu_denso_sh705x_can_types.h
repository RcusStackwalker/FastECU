#pragma once

#include <chrono>
#include <cstdint>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"

namespace fastecu::flash
{

// Wire setup for the ISO-15765-only Subaru Denso SH705x TCU family.
struct SubaruTcuDensoSh705xCanPlan
{
    std::uint32_t request_id{0x7E1};
    std::uint32_t response_id{0x7E9};
    int bitrate{500000};
    bool extended_id{false};
};

inline constexpr DensoBeefCanProfile kSubaruTcuDensoSh705xCanBeefProfile{.device = "TCU",
                                                                         .family = "TCU",
                                                                         .pdu_format = BeefPduFormat::kHex,
                                                                         .init_timeout =
                                                                             std::chrono::milliseconds{2000},
                                                                         .upper_hex_sizes = true};

} // namespace fastecu::flash
