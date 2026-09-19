#pragma once

#include <cstdint>

namespace fastecu::flash
{

enum class SubaruDensoSh7058CanSecurity
{
    Stock,
    EcuTek,
    RaceRom,
    RaceRomAlt,
    Cobb,
};

struct SubaruDensoSh7058CanPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
    SubaruDensoSh7058CanSecurity security{SubaruDensoSh7058CanSecurity::Stock};
};

} // namespace fastecu::flash
