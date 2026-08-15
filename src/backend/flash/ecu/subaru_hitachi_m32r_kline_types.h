#pragma once
#include <cstdint>

namespace fastecu::flash
{

enum class HitachiM32rKlineSessionMode
{
    Normal,
    Recovery,
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

} // namespace fastecu::flash
