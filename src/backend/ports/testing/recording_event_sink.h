#pragma once
#include "src/backend/ports/event_sink.h"

#include <string>
#include <vector>
#include <utility>

namespace fastecu
{

class RecordingEventSink : public IEventSink
{
  public:
    void log(LogLevel level, std::string_view message) override
    {
        logs.emplace_back(level, std::string{message});
    }
    void progress(int done, int total) override
    {
        progress_calls.emplace_back(done, total);
    }
    void notice(std::string_view message) override
    {
        notices.emplace_back(message);
    }
    std::vector<std::pair<fastecu::LogLevel, std::string>> logs;
    std::vector<std::pair<int, int>> progress_calls;
    std::vector<std::string> notices;
};

} // namespace fastecu
