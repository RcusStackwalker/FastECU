#pragma once
#include <cstdint>

namespace fastecu::flash
{

enum class DensoSecurityVariant
{
    Stock,
    EcuTek,
    Cobb,
    EcuTekRaceRom,
};

enum class EepromReadMode : std::uint8_t
{
    Mode2 = 2,
    Mode3 = 3,
    Mode4 = 4,
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

struct DensoSh705xEepromCanPlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false
};

} // namespace fastecu::flash
