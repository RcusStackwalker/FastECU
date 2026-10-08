#pragma once

#include <cstdint>
#include <optional>

#include "src/backend/logging/logger_model.h"
#include "src/backend/logging/logging_run_snapshot.h"
#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{
class LoggingRunSnapshot;

struct ResolvedLogSample
{
    LoggingMeasurementKind kind;
    LoggerIdentity identity;
    double numeric_value;
    std::uint8_t decimal_precision;
};

fastecu::Result<std::optional<ResolvedLogSample>> resolve_log_sample(const LoggingRunSnapshot& snapshot,
                                                                     const LogSample& sample);
} // namespace fastecu::logging
