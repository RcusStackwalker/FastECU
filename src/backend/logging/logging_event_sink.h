#pragma once

#include <span>

#include "src/backend/logging/logging_types.h"

class ILoggingEventSink
{
  public:
    virtual ~ILoggingEventSink() = default;
    virtual void state_changed(LoggingState state) = 0;
    virtual void samples(std::span<const LogSample> samples) = 0;
};
