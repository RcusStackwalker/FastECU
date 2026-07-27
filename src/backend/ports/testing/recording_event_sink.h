#pragma once
#include "src/backend/ports/event_sink.h"

#include <pair>
#include <string>
#include <vector>

namespace fastecu
{

class RecordingEventSink : public IEventSink
{
  public:
    void log(LogLevel, std::string_view) override
    {
        logs.emplace_back(level, std::string{message});
    }
    void progress(int, int) override
    {
        progress_calls.emplace_back(done, total);
    }
    void notice(std::string_view) override
    {
        notices.emplace_back(msg);
    }
    std::vector<std::pair<fastecu::LogLevel, std::string>> logs;
    std::vector<std::pair<int, int>> progress_calls;
    std::vector<std::string> notices;
};

} // namespace fastecu
