#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "apps/bench/bench_types.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/result.h"

namespace fastecu::bench
{

// The separator between chained steps. Steps chain inside one process because
// each process establishes one bootloader session: reconnecting re-runs 0x10
// and 0x27, so `unlock` and `erase` as two invocations would not reproduce
// what the desktop app does.
inline constexpr std::string_view kStepSeparator = ":";

struct GlobalOptions
{
    std::string port_name;
    bool json = false;
    bool verbose = false;
    int timeout_ms = 500;
    bool keep_going = false;
    bool no_connect = false;
    // Runs the third-party vendor diagnostic challenge before the bootload
    // session. Required by ROMs carrying that extension, which do not answer
    // a bare 0x10 0x85; stock ROMs must not be sent it.
    bool vendor_ext = false;
    bool script_stdin = false;
};

struct ParsedCommandLine
{
    GlobalOptions options;
    std::vector<StepSpec> steps;
};

// `args` excludes argv[0]. Global options may appear anywhere; everything else
// is grouped into steps split on kStepSeparator.
Result<ParsedCommandLine> parse_command_line(std::span<const std::string_view> args);

// Accepts "0x"-prefixed hex or plain decimal. Rejects empty input, trailing
// junk, and anything exceeding 32 bits.
Result<std::uint32_t> parse_u32(std::string_view text);

// Each token is exactly two hex digits.
Result<bytes::Bytes> parse_hex_bytes(std::span<const std::string> tokens);

} // namespace fastecu::bench
