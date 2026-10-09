#include "src/algorithms/diagnostics/dtc_parser.h"

#include "src/algorithms/diagnostics/dtc_tables.h"

#include <array>
#include <format>

namespace
{

std::string DefaultDtcMessage(std::uint16_t dtc)
{
    static constexpr std::array<char, 4> kPrefixes = {'P', 'C', 'B', 'U'};
    const std::size_t category = dtc >> 14;
    const std::uint16_t code = dtc & 0x3fff;

    return std::format("{}{:04x} - Unknown error code", kPrefixes[category], code);
}

} // namespace

std::string DtcDescription(std::uint16_t dtc, const std::unordered_map<int, std::string>& p_codes,
                           const std::unordered_map<int, std::string>& c_codes,
                           const std::unordered_map<int, std::string>& b_codes,
                           const std::unordered_map<int, std::string>& u_codes)
{
    const std::string fallback = DefaultDtcMessage(dtc);

    const std::unordered_map<int, std::string> *table = nullptr;
    switch (dtc >> 14)
    {
    case 0x00:
        table = &p_codes;
        break;
    case 0x01:
        table = &c_codes;
        break;
    case 0x02:
        table = &b_codes;
        break;
    case 0x03:
        table = &u_codes;
        break;
    default:
        return fallback;
    }

    // Tables are keyed by the 14-bit code, not the full value -- the top two
    // bits already selected which table to consult above.
    const auto it = table->find(dtc & 0x3fff);
    return it != table->end() ? it->second : fallback;
}

std::string DtcDescription(std::uint16_t dtc)
{
    return DtcDescription(dtc, DtcPCodes(), DtcCCodes(), DtcBCodes(), DtcUCodes());
}
