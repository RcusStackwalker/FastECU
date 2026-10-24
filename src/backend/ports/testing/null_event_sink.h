#pragma once

#include <string_view>

#include "src/backend/ports/event_sink.h"

namespace fastecu
{

// A no-op sink for tests that do not inspect events.
class NullEventSink : public IEventSink
{
  public:
    void Log(LogLevel, std::string_view) override
    {
    }
    void Progress(int, int) override
    {
    }
    void Notice(std::string_view) override
    {
    }
};

} // namespace fastecu
