#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace bytes
{

// Reading a fixed-size char buffer that a foreign driver filled.
//
// J2534's PassThruReadVersion takes no length for its out-parameters, so a
// driver that fills the whole buffer leaves no terminator and strlen() runs
// off the end. These helpers stop at the buffer's end whether or not one is
// there. Read such a buffer only through them -- never strlen()/strcpy().

// Contents up to the first NUL, or the whole buffer if it has none. The view
// aliases the caller's buffer and does not outlive it.
[[nodiscard]] inline std::string_view fromFixedBuffer(std::span<const char> buffer) noexcept
{
    const auto terminator = std::ranges::find(buffer, '\0');
    return {buffer.data(), static_cast<std::size_t>(terminator - buffer.begin())};
}

// Same, minus the final character: the J2534 version strings carry a trailing
// byte callers do not want to display. Empty in, empty out.
[[nodiscard]] inline std::string_view fromFixedBufferDroppingLast(std::span<const char> buffer) noexcept
{
    std::string_view text = fromFixedBuffer(buffer);
    if (!text.empty())
    {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace bytes
