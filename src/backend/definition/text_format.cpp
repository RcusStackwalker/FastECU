#include "src/backend/definition/text_format.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace fastecu::definition
{
namespace
{
// UTF-8 encodings of Unicode White_Space, independent of locale.
constexpr auto kWhitespace = std::to_array<std::string_view>({" ",
                                                              "\t",
                                                              "\n",
                                                              "\v",
                                                              "\f",
                                                              "\r",
                                                              "\xc2\x85",
                                                              "\xc2\xa0",
                                                              "\xe1\x9a\x80",
                                                              "\xe2\x80\x80",
                                                              "\xe2\x80\x81",
                                                              "\xe2\x80\x82",
                                                              "\xe2\x80\x83",
                                                              "\xe2\x80\x84",
                                                              "\xe2\x80\x85",
                                                              "\xe2\x80\x86",
                                                              "\xe2\x80\x87",
                                                              "\xe2\x80\x88",
                                                              "\xe2\x80\x89",
                                                              "\xe2\x80\x8a",
                                                              "\xe2\x80\xa8",
                                                              "\xe2\x80\xa9",
                                                              "\xe2\x80\xaf",
                                                              "\xe2\x81\x9f",
                                                              "\xe3\x80\x80"});

} // namespace

std::string_view trim_header_text(std::string_view text)
{
    for (;;)
    {
        const auto space = std::ranges::find_if(kWhitespace, [text](auto value) { return text.starts_with(value); });
        if (space == kWhitespace.end())
        {
            break;
        }
        text.remove_prefix(space->size());
    }
    for (;;)
    {
        const auto space = std::ranges::find_if(kWhitespace, [text](auto value) { return text.ends_with(value); });
        if (space == kWhitespace.end())
        {
            break;
        }
        text.remove_suffix(space->size());
    }
    return text;
}

} // namespace fastecu::definition
