#pragma once

#include <cstdint>

namespace fastecu::flash
{
// Step 5 tail, wave 7. Bootmode Write is two attempts: the kernel upload into
// the M32R boot ROM, then erase and program through that kernel. Each attempt
// is its own family so each executor's check_family() rejects the other's
// plan. The ROM and kernel sizes come from the plan's transfer region.
struct SubaruUnisiaJecsM32rBootModeKernelPlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

struct SubaruUnisiaJecsM32rBootModeProgramPlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};
} // namespace fastecu::flash
