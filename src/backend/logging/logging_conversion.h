#pragma once

#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

fastecu::Result<LogSample> convert_sample(const LoggingSession& session,
                                          const ProtocolSample& raw);

} // namespace fastecu::logging
