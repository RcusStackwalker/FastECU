#include "src/backend/calibration/scaling_internal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>

namespace fastecu::calibration::internal
{

// Reproduces QString::number(value, 'g', precision). Qt's 'g' and std::format's
// 'g' agree on trailing-zero stripping and exponent thresholds across the range
// these ROMs produce -- pinned by FormattingMatchesCapturedQtGroundTruth, which
// compares against real Qt output rather than assuming compatibility.
std::string format_like_qt_g(double value, int precision)
{
    if (value == 0.0)
    {
        value = 0.0; // normalizes -0.0 to +0.0, as Qt does
    }
    // std::format throws on a negative precision where "%.*g" silently fell
    // back to the default. Precision reaches here from a uint8_t field, so the
    // clamp is unreachable; 0 and 1 render identically either way.
    return std::format("{:.{}g}", value, std::max(precision, 1));
}

// Sign-extends an assembled `width`-byte value to a full int32. Widths of 4 or
// more are already full-width; width 0 cannot occur (storage_byte_size floors
// at 1) but is handled rather than shifted out of range.
std::int32_t sign_extend(std::uint32_t raw, std::uint32_t width)
{
    if (width == 0 || width >= 4)
    {
        return static_cast<std::int32_t>(raw);
    }
    const std::uint32_t sign_bit = 1U << (width * 8 - 1);
    if ((raw & sign_bit) == 0)
    {
        return static_cast<std::int32_t>(raw);
    }
    return static_cast<std::int32_t>(raw | ~((sign_bit << 1) - 1));
}

bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result)
{
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result)
{
    if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
    {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool byte_window_fits(bytes::ByteView data, std::uint64_t address, std::uint64_t width)
{
    const std::uint64_t size = data.size();
    return address <= size && width <= size - address;
}

} // namespace fastecu::calibration::internal
