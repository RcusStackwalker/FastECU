#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Wave 6b-2. Legacy connect_bootloader():269 picks the ECUTEK seed-key
// transformation when the flash method (the protocol name) ends in "_ecutek".
enum class SubaruDensoSh705xKlineSeedKey
{
    Stock,
    EcuTek,
};

struct SubaruDensoSh705xKlinePlan
{
    int initial_baud;       // 4800, execute():72
    std::uint8_t tester_id; // 0xF0, execute():73
    std::uint8_t target_id; // 0x10, execute():74
    SubaruDensoSh705xKlineSeedKey seed_key;
};

} // namespace fastecu::flash
