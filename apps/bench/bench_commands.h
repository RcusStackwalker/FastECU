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

// Executes one step and always returns its outcome. Failures are represented
// by ok=false plus error_kind/error_detail so traffic already observed is not
// discarded when the caller decides whether to stop the chain.
CommandOutcome run_step(BenchContext& context, const StepSpec& step);

// RoutineControl 224's reply payload ([routine-id][status]). Names both code
// paths that can produce status 1 because the reply cannot distinguish them.
std::string decode_erase_reply(bytes::ByteView payload);

// RoutineControl 225's reply payload ([routine-id][status]).
std::string decode_crc_reply(bytes::ByteView payload);

} // namespace fastecu::bench
