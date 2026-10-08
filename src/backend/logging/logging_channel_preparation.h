#pragma once

#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{
fastecu::Result<LoggingChannel> prepare_logging_channel(const LoggerParameter& parameter, LoggingProtocolId protocol);
Result<LoggingChannel> prepare_logging_switch(const LoggerSwitch& source, LoggingProtocolId protocol);
} // namespace fastecu::logging
