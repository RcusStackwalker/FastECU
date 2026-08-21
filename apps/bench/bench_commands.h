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

// A fully validated step. File-backed upload bytes are captured during
// preparation so execution cannot observe a different file after the ECU
// session has opened.
struct PreparedStep
{
    StepSpec spec;
    std::optional<bytes::Bytes> upload_payload;
};

// Validates everything that can be checked without an ECU session and loads
// file-backed payloads exactly once.
Result<PreparedStep> prepare_step(IBenchFiles& files, const StepSpec& step);

// Executes one step and always returns its outcome. Failures are represented
// by ok=false plus error_kind/error_detail so traffic already observed is not
// discarded when the caller decides whether to stop the chain.
CommandOutcome run_step(BenchContext& context, const StepSpec& step);
CommandOutcome run_step(BenchContext& context, const PreparedStep& step);

// RoutineControl 224's reply payload ([routine-id][status]). Names both code
// paths that can produce status 1 because the reply cannot distinguish them.
std::string decode_erase_reply(bytes::ByteView payload);

// RoutineControl 225's reply payload ([routine-id][status]).
std::string decode_crc_reply(bytes::ByteView payload);

} // namespace fastecu::bench
