#include "src/algorithms/diagnostics/nrc_parser.h"

std::string nrc_description(bytes::ByteView nrc, const std::unordered_map<int, std::string>& codes)
{
    if (nrc.size() < 3 || nrc[0] != 0x7f)
    {
        return "Not a valid answer";
    }

    const auto it = codes.find(static_cast<int>(nrc[2]));
    if (it != codes.end())
    {
        return it->second;
    }
    return "Unknown error code";
}
