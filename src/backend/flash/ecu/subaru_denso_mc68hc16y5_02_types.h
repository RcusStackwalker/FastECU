#pragma once
#include <array>
#include <cstdint>

namespace fastecu::flash
{

// MC68HC16Y5_02, wave 2. tester_id/target_id are NOT carried here: legacy
// flash_ecu_subaru_denso_mc68hc16y5_02_operation.h declares them but the
// .cpp never reads them after execute() assigns 0xf0/0x10 (verified by
// grep across every method) -- dead members, not ported.
struct SubaruDensoMc68hc16y5_02Plan
{
    int connect_baud;                          // 9600 (connect_bootloader)
    int kernel_baud;                           // 11700 (_ecutek) or 9600 (stock/_cobb)
    std::uint8_t encryption_xor;               // 0x51 (_ecutek) or 0x55 (stock/_cobb)
    std::uint16_t kernel_magic;                // 0x3940 (_ecutek) or 0x3941 (stock/_cobb)
    std::array<std::uint8_t, 3> bootloader_ok; // WRX02 init OK response: stock/_cobb
                                               // {0x4D,0x00,0xB3}, _ecutek {0x4C,0x00,0xB4}
};

} // namespace fastecu::flash
