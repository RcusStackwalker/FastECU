#pragma once
#include <string>

#include "apps/bench/bench_types.h"
#include "src/backend/ports/error.h"

namespace fastecu::bench
{

// Human-readable rendering: one step per block, first/last tx/rx as spaced hex.
std::string format_text(const CommandOutcome& outcome);

// One flat JSON object per step, including first/last traffic, count and
// elapsed time. Hex has no separators so an agent can slice it directly.
// Emitted on stdout; all logging goes to stderr.
std::string format_json(const CommandOutcome& outcome);

// Distinct non-zero code per ErrorKind so an agent branches on the exit status
// without parsing prose. 0 is reserved for success.
int exit_code_for(ErrorKind kind);

} // namespace fastecu::bench
