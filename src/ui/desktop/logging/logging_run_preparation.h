#pragma once

#include <string>
#include <string_view>

#include "src/backend/logging/logging_run_snapshot.h"
#include "src/backend/ports/result.h"

namespace fastecu::ui
{
struct LoggingRunChoices
{
    std::string_view saved_protocol;
    std::string_view ssm_definition_filter;
    logging::LoggingTarget target;
};

struct PreparedLoggingRun
{
    std::string registration_key;
    logging::LoggingRunSnapshot snapshot;
};

// Desktop defaults and backend validation for one run, without I/O or retained
// references. Every saved protocol except MUT_DMA/CDBG uses the supplied SSM
// definition filter. Backend preparation errors propagate unchanged.
Result<PreparedLoggingRun> prepareLoggingRun(const logging::LoggerModel& model, LoggingRunChoices choices);
} // namespace fastecu::ui
