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
#include "src/backend/ports/result.h"

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
const CommandSpec *find_command(CommandId id);

struct StepSpec
{
    CommandId id;
    std::vector<std::string> args;
    bool destructive_ack = false;
};

// Everything the command table alone decides about a step: its argument count,
// and that a destructive command carries its --destructive acknowledgement.
// The parser, the preparer and the last gate before the wire all call this, so
// one table row governs all three and the messages cannot drift apart.
Status validate_against_table(const CommandSpec& spec, const StepSpec& step);

// Diagnostic evidence for one session operation. `tx`/`rx` are the first
// request and response PDUs; `last_tx`/`last_rx` are the final pair. The two
// pairs are identical for a single exchange. Responses include their positive
// or negative UDS service byte. An empty RX means no complete PDU arrived.
struct TrafficEvidence
{
    std::size_t exchange_count = 0;
    bytes::Bytes tx;
    bytes::Bytes rx;
    bytes::Bytes last_tx;
    bytes::Bytes last_rx;
    std::uint64_t elapsed_ms = 0;
};

// Accumulates one operation's evidence into a running total: the first
// exchange's pair is kept, the last pair overwrites it, and counts and elapsed
// time add up. Templated on the accumulator because CommandOutcome carries the
// same six fields inline, so both it and TrafficEvidence are valid totals.
template <class Accumulator> void append_traffic(Accumulator& total, const TrafficEvidence& next)
{
    if (next.exchange_count == 0)
    {
        return;
    }
    if (total.exchange_count == 0)
    {
        total.tx = next.tx;
        total.rx = next.rx;
    }
    total.last_tx = next.last_tx;
    total.last_rx = next.last_rx;
    total.exchange_count += next.exchange_count;
    total.elapsed_ms += next.elapsed_ms;
}

// What one executed step produced. `data` is command data (for example the
// complete memory range returned by a chunked read); traffic fields are kept
// separately so multi-exchange diagnostic evidence is never confused with
// accumulated payload data.
struct CommandOutcome
{
    std::string step;
    std::size_t exchange_count = 0;
    bytes::Bytes tx;
    bytes::Bytes rx;
    bytes::Bytes last_tx;
    bytes::Bytes last_rx;
    bytes::Bytes data;
    std::uint64_t elapsed_ms = 0;
    std::optional<double> vbatt;
    bool ok = true;
    std::string note;
    std::optional<ErrorKind> error_kind;
    std::string error_detail;
};

} // namespace fastecu::bench
