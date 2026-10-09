#include "src/backend/diagnostics/terminal_script.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <string_view>
#include <system_error>

namespace fastecu::diagnostics
{

namespace
{
constexpr std::string_view kDelayPrefix = "delay(";
constexpr std::string_view kWhitespace = " \t\r\n";

Error line_error(std::size_t line, const std::string& detail)
{
    return Error{ErrorKind::kInvalidConfig, std::format("line {}: {}", line, detail)};
}

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kWhitespace) - first + 1);
}

Result<TerminalDelayStep> parse_delay(std::string_view entry, std::size_t line)
{
    if (!entry.starts_with(kDelayPrefix) || !entry.ends_with(')'))
    {
        return std::unexpected(line_error(line, std::format("expected delay(<milliseconds>), got '{}'", entry)));
    }
    const std::string_view digits = entry.substr(kDelayPrefix.size(), entry.size() - kDelayPrefix.size() - 1);
    std::uint64_t milliseconds = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), milliseconds);
    const bool all_digits = !digits.empty() && std::isdigit(static_cast<unsigned char>(digits.front())) != 0 &&
                            std::isdigit(static_cast<unsigned char>(digits.back())) != 0 &&
                            end == digits.data() + digits.size();
    if (!all_digits || ec == std::errc::invalid_argument)
    {
        return std::unexpected(line_error(line, std::format("delay needs decimal milliseconds, got '{}'", digits)));
    }
    if (ec == std::errc::result_out_of_range || milliseconds > static_cast<std::uint64_t>(kMaxTerminalDelay.count()))
    {
        return std::unexpected(
            line_error(line, std::format("delay exceeds the {} ms maximum", kMaxTerminalDelay.count())));
    }
    return TerminalDelayStep{std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds))};
}

Result<TerminalMessageStep> parse_message(std::string_view entry, std::size_t line)
{
    TerminalMessageStep step;
    while (!entry.empty())
    {
        const auto end = entry.find_first_of(kWhitespace);
        const std::string_view token = entry.substr(0, end);
        unsigned value = 0;
        const auto [stop, ec] = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        const bool valid = token.size() <= 2 && ec == std::errc{} && stop == token.data() + token.size() &&
                           std::isxdigit(static_cast<unsigned char>(token.front())) != 0;
        if (!valid)
        {
            return std::unexpected(line_error(line, std::format("'{}' is not a hex byte (one or two digits)", token)));
        }
        step.payload.push_back(static_cast<bytes::Byte>(value));
        entry = end == std::string_view::npos ? std::string_view{} : trim(entry.substr(end));
    }
    return step;
}
} // namespace

Result<std::vector<TerminalStep>> parse_terminal_script(std::span<const std::string> lines)
{
    std::vector<TerminalStep> steps;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string_view entry = trim(lines[index]);
        if (entry.empty())
        {
            continue;
        }
        if (entry.starts_with("delay"))
        {
            auto delay = parse_delay(entry, index + 1);
            if (!delay)
            {
                return std::unexpected(delay.error());
            }
            steps.emplace_back(*delay);
            continue;
        }
        auto message = parse_message(entry, index + 1);
        if (!message)
        {
            return std::unexpected(message.error());
        }
        steps.emplace_back(std::move(*message));
    }
    if (steps.empty())
    {
        return fail(ErrorKind::kInvalidConfig, "script has no steps");
    }
    return steps;
}

} // namespace fastecu::diagnostics
