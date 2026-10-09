#pragma once

#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

class LoggingSession;

fastecu::Result<LogSample> ConvertSample(const LoggingSession& session, const ProtocolSample& raw);

} // namespace fastecu::logging
