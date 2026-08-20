#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/error.h"

namespace fastecu::bench
{

enum class CommandId
{
    Ports,
    Connect,
    Read,
    Dump,
    CrcCheck,
    Send,
    SendRaw,
    Unlock,
    Erase,
    Download,
    UploadRoutine,
};

// `max_args` is kUnbounded for the variadic hex commands.
inline constexpr std::size_t kUnbounded = static_cast<std::size_t>(-1);

struct CommandSpec
{
    CommandId id;
    std::string_view name;
    // Requires an explicit --destructive on its own step. Checked at parse
    // time, before the port is opened, so a chain whose third step is ungated
    // fails before connecting and unlocking rather than halfway through.
    bool destructive;
    std::size_t min_args;
    std::size_t max_args;
};

std::span<const CommandSpec> command_table();
const CommandSpec *find_command(std::string_view name);

struct StepSpec
{
    CommandId id;
    std::vector<std::string> args;
    bool destructive_ack = false;
};

// What one executed step produced. `tx`/`rx` hold the first request and the
// accumulated reply payload; for a chunked read `note` carries the chunk count
// rather than every individual request being recorded.
struct CommandOutcome
{
    std::string step;
    bytes::Bytes tx;
    bytes::Bytes rx;
    std::uint64_t elapsed_ms = 0;
    std::optional<double> vbatt;
    bool ok = true;
    std::string note;
    std::optional<ErrorKind> error_kind;
    std::string error_detail;
};

} // namespace fastecu::bench
