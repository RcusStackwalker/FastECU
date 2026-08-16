#include "src/algorithms/diagnostics/dtc_parser.h"

#include "src/algorithms/diagnostics/dtc_tables.h"

#include <array>
#include <format>

namespace
{

std::string default_dtc_message(std::uint16_t dtc)
{
    static constexpr std::array<char, 4> prefixes = {'P', 'C', 'B', 'U'};
    const std::size_t category = dtc >> 14;
    const std::uint16_t code = dtc & 0x3fff;

    return std::format("{}{:04x} - Unknown error code", prefixes[category], code);
}

} // namespace

std::string dtc_description(std::uint16_t dtc, const std::unordered_map<int, std::string>& pCodes,
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

    // Tables are keyed by the 14-bit code, not the full value -- the top two
    // bits already selected which table to consult above.
    const auto it = table->find(dtc & 0x3fff);
    return it != table->end() ? it->second : fallback;
}

std::string dtc_description(std::uint16_t dtc)
{
    return dtc_description(dtc, dtc_p_codes(), dtc_c_codes(), dtc_b_codes(), dtc_u_codes());
}
