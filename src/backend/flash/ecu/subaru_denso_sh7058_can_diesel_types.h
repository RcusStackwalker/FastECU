#pragma once

#include <cstdint>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"

namespace fastecu::flash
{

struct SubaruDensoSh7058CanDieselPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
};

inline constexpr DensoBeefCanProfile kSubaruDensoSh7058CanDieselBeefProfile{.device = "ECU",
                                                                            .family = "diesel",
                                                                            .wrong_reply =
                                                                                BeefWrongReply::kNotValidAnswer,
                                                                            .pdu_format = BeefPduFormat::kDescribe};

} // namespace fastecu::flash
