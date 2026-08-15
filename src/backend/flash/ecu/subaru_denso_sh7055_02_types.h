#pragma once
#include <cstdint>

namespace fastecu::flash
{

// SH7055_02, wave 2. Unlike MC68, tester_id/target_id ARE live -- the one
// surviving SSM-framed exchange (SID 0xBF ECU-ID read, read-only, Read
// operation only) uses them via SsmProtocol::addHeader().
struct SubaruDensoSh7055_02Plan
{
    std::uint8_t tester_id; // 0xf0
    std::uint8_t target_id; // 0x10
    bool read_ecu_id;       // true iff FlashOperation::Read
};

} // namespace fastecu::flash
