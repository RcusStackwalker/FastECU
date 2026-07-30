#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace fastecu::definition
{

inline std::string hex_text(std::uint64_t value)
{
    return std::format("0x{:x}", value);
}

} // namespace fastecu::definition
