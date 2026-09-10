#pragma once

#include <chrono>
#include <concepts>
#include <limits>
#include <utility>

namespace fastecu
{

// Converts a duration to the integral millisecond count an integral wire API
// wants, clamping to [0, max of T].
//
// Saturating rather than asserting: a too-long timeout should wait as long as
// the wire type allows, never abort a flash mid-write. Clamping negatives to
// zero matches every caller's existing "no wait" reading of a non-positive
// timeout.
template <std::integral T> constexpr T saturating_ms(std::chrono::milliseconds duration) noexcept
{
    using Limits = std::numeric_limits<T>;
    const auto count = duration.count();
    if (count <= 0)
    {
        return T{0};
    }
    // count is a signed 64-bit rep; compare in that width so a 64-bit T
    // maximum does not itself overflow the comparison.
    if (std::cmp_greater(count, Limits::max()))
    {
        return Limits::max();
    }
    return static_cast<T>(count);
}

} // namespace fastecu
