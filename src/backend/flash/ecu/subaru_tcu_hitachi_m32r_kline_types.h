#pragma once
#include <cstdint>

namespace fastecu::flash
{
// Legacy: flash_tcu_subaru_hitachi_m32r_kline_operation.{h,cpp}, read path
// only. Its execute() "test_write"/"write" branch logged "Not yet
// implemented" and then returned connect_bootloader()'s STATUS_SUCCESS,
// reporting a successful write that wrote nothing; this plan rejects both
// operations instead. See the wave 6a-1 plan's "Deliberate Divergences".
struct SubaruTcuHitachiM32rKlinePlan
{
    std::uint8_t tester_id;   // 0xf0
    std::uint8_t target_id;   // 0x18, not the ECU family's 0x10
    int baud;                 // 4800, never changed mid-session
    std::uint32_t block_size; // 96, not the ECU family's 128
};
} // namespace fastecu::flash
