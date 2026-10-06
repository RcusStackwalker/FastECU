#pragma once
#include <chrono>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/result.h"

namespace fastecu::diagnostics
{

// Longest pause a single delay step may request.
inline constexpr std::chrono::milliseconds kMaxTerminalDelay = std::chrono::hours(1);

// Sends one frame and reads its response.
struct TerminalMessageStep
{
    bytes::Bytes payload;

    bool operator==(const TerminalMessageStep&) const = default;
};

// Only pauses: sends nothing and reads nothing.
struct TerminalDelayStep
{
    std::chrono::milliseconds duration{0};

    bool operator==(const TerminalDelayStep&) const = default;
};

using TerminalStep = std::variant<TerminalMessageStep, TerminalDelayStep>;

// One entry per line: space-separated hex bytes (one or two digits each), or
// `delay(<milliseconds>)`. Blank lines are skipped. The whole script is
// validated before any step is returned, so a caller never runs part of one.
// Errors are InvalidConfig and name the offending 1-based line.
Result<std::vector<TerminalStep>> parse_terminal_script(std::span<const std::string> lines);

} // namespace fastecu::diagnostics
