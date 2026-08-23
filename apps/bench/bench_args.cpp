#include "apps/bench/bench_args.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

namespace fastecu::bench
{
namespace
{

bool isKnownDestructivePdu(bytes::ByteView pdu)
{
    if (pdu.empty())
    {
        return false;
    }
    if (pdu[0] == MitsuColtCan::kServiceRequestReflash || pdu[0] == MitsuColtCan::kServiceRequestDownload ||
        pdu[0] == MitsuColtCan::kServiceTransferData)
    {
        return true;
    }
    return pdu.size() >= 2 && pdu[0] == MitsuColtCan::kServiceRoutineControl && pdu[1] == MitsuColtCan::kRoutineErase;
}

Result<StepSpec> makeStep(const std::vector<std::string>& tokens)
{
    if (tokens.empty())
    {
        return fail(ErrorKind::InvalidConfig, "empty step");
    }

    std::vector<std::string> args;
    bool destructive_ack = false;
    for (std::size_t index = 1; index < tokens.size(); ++index)
    {
        if (tokens[index] == "--destructive")
        {
            destructive_ack = true;
            continue;
        }
        // Every other token, including upload-routine's own "--from <path>",
        // passes through verbatim as an ordinary step argument -- only
        // --destructive is a flag at this layer.
        args.push_back(tokens[index]);
    }

    const CommandSpec *spec = find_command(tokens.front());
    if (spec == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, std::format("unknown command: {}", tokens.front()));
    }
    if (args.size() < spec->min_args || (spec->max_args != kUnbounded && args.size() > spec->max_args))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} takes {}..{} arguments, got {}", spec->name, spec->min_args,
                                spec->max_args == kUnbounded ? std::string("*") : std::to_string(spec->max_args),
                                args.size()));
    }
    // Validated here, at parse time, rather than at execution: a chain whose
    // later step is ungated must fail before the port is opened, not after it
    // has already connected and unlocked.
    if (spec->destructive && !destructive_ack)
    {
        return fail(ErrorKind::InvalidConfig, std::format("{} needs --destructive", spec->name));
    }
    if (!spec->destructive && destructive_ack)
    {
        return fail(ErrorKind::InvalidConfig, std::format("{} is not destructive", spec->name));
    }
    if (spec->id == CommandId::Send || spec->id == CommandId::SendRaw)
    {
        const Result<bytes::Bytes> pdu = parse_hex_bytes(args);
        if (!pdu.has_value())
        {
            return std::unexpected(pdu.error());
        }
        if (isKnownDestructivePdu(*pdu))
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("{} cannot send a known destructive PDU; use the named destructive command", spec->name));
        }
    }

    return StepSpec{.id = spec->id, .args = std::move(args), .destructive_ack = destructive_ack};
}

} // namespace

Result<std::uint32_t> parse_u32(std::string_view text)
{
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty())
    {
        return fail(ErrorKind::InvalidConfig, "empty number");
    }

    std::uint64_t value = 0;
    const auto *const end = text.data() + text.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) -- from_chars takes [first,last); end has the length.
    const auto [stopped, error] = std::from_chars(text.data(), end, value, base);
    if (error != std::errc{} || stopped != end)
    {
        return fail(ErrorKind::InvalidConfig, std::format("not a number: {}", text));
    }
    if (value > 0xFFFFFFFFull)
    {
        return fail(ErrorKind::InvalidConfig, std::format("does not fit in 32 bits: {}", text));
    }
    return static_cast<std::uint32_t>(value);
}

Result<bytes::Bytes> parse_hex_bytes(std::span<const std::string> tokens)
{
    bytes::Bytes out;
    out.reserve(tokens.size());
    for (const std::string& token : tokens)
    {
        if (token.size() != 2)
        {
            return fail(ErrorKind::InvalidConfig, std::format("not a hex byte: {}", token));
        }
        std::uint32_t value = 0;
        const auto *const end = token.data() + token.size();
        const auto [stopped, error] = std::from_chars(token.data(), end, value, 16);
        if (error != std::errc{} || stopped != end)
        {
            return fail(ErrorKind::InvalidConfig, std::format("not a hex byte: {}", token));
        }
        out.push_back(static_cast<bytes::Byte>(value));
    }
    return out;
}

Result<ParsedCommandLine> parse_command_line(std::span<const std::string_view> args)
{
    ParsedCommandLine parsed;
    std::vector<std::vector<std::string>> groups{{}};

    for (std::size_t index = 0; index < args.size(); ++index)
    {
        const std::string_view arg = args[index];
        const auto needsValue = [&](std::string_view& out) -> Status
        {
            if (index + 1 >= args.size())
            {
                return fail(ErrorKind::InvalidConfig, std::format("{} needs a value", arg));
            }
            out = args[++index];
            return {};
        };

        if (arg == kStepSeparator)
        {
            groups.emplace_back();
            continue;
        }
        if (arg == "--json")
        {
            parsed.options.json = true;
            continue;
        }
        if (arg == "--verbose")
        {
            parsed.options.verbose = true;
            continue;
        }
        if (arg == "--keep-going")
        {
            parsed.options.keep_going = true;
            continue;
        }
        if (arg == "--no-connect")
        {
            parsed.options.no_connect = true;
            continue;
        }
        if (arg == "--vendor-ext")
        {
            parsed.options.vendor_ext = true;
            continue;
        }
        if (arg == "--stats")
        {
            parsed.options.stats = true;
            continue;
        }
        if (arg == "--script")
        {
            std::string_view value;
            if (const Status ok = needsValue(value); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            if (value != "-")
            {
                return fail(ErrorKind::InvalidConfig, "--script only accepts '-' (stdin)");
            }
            parsed.options.script_stdin = true;
            continue;
        }
        if (arg == "--port")
        {
            std::string_view value;
            if (const Status ok = needsValue(value); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            parsed.options.port_name = value;
            continue;
        }
        if (arg == "--timeout")
        {
            std::string_view value;
            if (const Status ok = needsValue(value); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            const Result<std::uint32_t> timeout = parse_u32(value);
            if (!timeout.has_value())
            {
                return std::unexpected(timeout.error());
            }
            if (*timeout > std::numeric_limits<std::uint16_t>::max())
            {
                return fail(ErrorKind::InvalidConfig, "timeout must not exceed 65535 ms");
            }
            parsed.options.timeout_ms = static_cast<int>(*timeout);
            continue;
        }

        groups.back().emplace_back(arg);
    }

    if (parsed.options.script_stdin)
    {
        // Steps come from stdin instead; main.cpp re-enters parse_command_line
        // per line. Any step tokens on the command line would be ambiguous.
        for (const std::vector<std::string>& group : groups)
        {
            if (!group.empty())
            {
                return fail(ErrorKind::InvalidConfig, "--script - takes no steps on the command line");
            }
        }
        return parsed;
    }

    for (const std::vector<std::string>& group : groups)
    {
        Result<StepSpec> step = makeStep(group);
        if (!step.has_value())
        {
            return std::unexpected(step.error());
        }
        parsed.steps.push_back(std::move(*step));
    }
    if (parsed.steps.empty())
    {
        return fail(ErrorKind::InvalidConfig, "no steps given");
    }
    // main.cpp handles `ports` before any transport exists, which only works
    // if it is the sole step: chaining it with a step that needs a session
    // would otherwise open the device just to run a command that promises
    // never to.
    if (parsed.steps.size() > 1 &&
        std::ranges::any_of(parsed.steps, [](const StepSpec& step) { return step.id == CommandId::Ports; }))
    {
        return fail(ErrorKind::InvalidConfig, "ports cannot be chained with other commands");
    }
    return parsed;
}

} // namespace fastecu::bench
