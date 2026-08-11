#pragma once
#include "src/backend/ports/event_sink.h"

#include <string>
#include <vector>
#include <utility>

namespace fastecu
{

struct RecordedPhaseProgress
{
    std::string phase_name;
    int phase_index;
    int phase_count;
    int done;
    int total;
};

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
    void phase_progress(const PhaseProgressEvent& event) override
    {
        IEventSink::phase_progress(event);
        phase_progress_calls.push_back({std::string(event.phase_name), event.phase_index,
                                        event.phase_count, event.done, event.total});
    }
    void notice(std::string_view message) override
    {
        notices.emplace_back(message);
    }
    std::vector<std::pair<fastecu::LogLevel, std::string>> logs;
    std::vector<std::pair<int, int>> progress_calls;
    std::vector<RecordedPhaseProgress> phase_progress_calls;
    std::vector<std::string> notices;
};

} // namespace fastecu
