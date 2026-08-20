#pragma once
#include <string>

#include "apps/bench/bench_args.h"
#include "apps/bench/bench_session_interface.h"
#include "apps/bench/bench_types.h"

namespace fastecu::bench
{

struct BenchContext
{
    IBenchSession& session;
    IBenchFiles& files;
    const GlobalOptions& options;
};

// Executes one step. A failure returns the error rather than an unsuccessful
// outcome, so the caller can stop the chain; the outcome's own `ok` field is
// for reporting, not control flow.
Result<CommandOutcome> run_step(BenchContext& context, const StepSpec& step);

// RoutineControl 224's reply payload ([routine-id][status]). Names both code
// paths that can produce status 1 because the reply cannot distinguish them.
std::string decode_erase_reply(bytes::ByteView payload);

// RoutineControl 225's reply payload ([routine-id][status]).
std::string decode_crc_reply(bytes::ByteView payload);

} // namespace fastecu::bench
