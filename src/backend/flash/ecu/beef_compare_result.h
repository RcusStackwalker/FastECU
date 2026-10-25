#pragma once

#include <cstddef>
#include <vector>

namespace fastecu::flash
{

// Which erase regions the ECU's CRC disagrees with the image on.
struct CompareResult
{
    std::vector<bool> modified;
    std::size_t changed_count{};
};

} // namespace fastecu::flash
