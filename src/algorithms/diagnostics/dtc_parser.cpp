#include "src/algorithms/diagnostics/dtc_parser.h"

#include <array>
#include <cstdio>

namespace
{

std::string default_dtc_message(std::uint16_t dtc)
{
    static constexpr std::array<char, 4> prefixes = {'P', 'C', 'B', 'U'};
    const std::size_t category = dtc >> 14;
    const std::uint16_t code = dtc & 0x3fff;

    char hex[8];
    std::snprintf(hex, sizeof(hex), "%04x", code);
    return std::string(1, prefixes[category]) + hex + " - Unknown error code";
}

} // namespace

std::string dtc_description(std::uint16_t dtc,
                            const std::unordered_map<int, std::string>& pCodes,
                            const std::unordered_map<int, std::string>& cCodes,
                            const std::unordered_map<int, std::string>& bCodes,
                            const std::unordered_map<int, std::string>& uCodes)
{
    const std::string fallback = default_dtc_message(dtc);

    const std::unordered_map<int, std::string> *table = nullptr;
    switch (dtc >> 14)
    {
    case 0x00:
        table = &pCodes;
        break;
    case 0x01:
        table = &cCodes;
        break;
    case 0x02:
        table = &bCodes;
        break;
    case 0x03:
        table = &uCodes;
        break;
    default:
        return fallback;
    }

    const auto it = table->find(dtc);
    return it != table->end() ? it->second : fallback;
}
