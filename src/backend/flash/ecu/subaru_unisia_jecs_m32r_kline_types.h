#pragma once

#include <cstdint>

namespace fastecu::flash
{
// Step 5 tail, wave 6c-3. Framed SSM over plain K-Line; the ROM size comes
// from the plan's transfer region, so only the session parameters live here.
struct SubaruUnisiaJecsM32rKlinePlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};
} // namespace fastecu::flash
