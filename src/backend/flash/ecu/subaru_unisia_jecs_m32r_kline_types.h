#pragma once

#include <cstdint>

namespace fastecu::flash
{
// Framed SSM over plain K-Line; the ROM size comes
// from the plan's transfer region, so only the session parameters live here.
struct SubaruUnisiaJecsM32rKlinePlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;

    bool operator==(const SubaruUnisiaJecsM32rKlinePlan&) const = default;
};
} // namespace fastecu::flash
