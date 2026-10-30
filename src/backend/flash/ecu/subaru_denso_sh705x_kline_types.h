#pragma once
#include <cstdint>

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

} // namespace fastecu::flash
