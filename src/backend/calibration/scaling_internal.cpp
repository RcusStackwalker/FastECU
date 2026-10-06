#include "src/backend/calibration/scaling_internal.h"

#include <limits>

namespace fastecu::calibration::internal
{

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
