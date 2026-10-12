#pragma once
#include "src/backend/ports/event_sink.h"

#include <ostream>
#include <string>
#include <vector>
#include <utility>

namespace fastecu
{

// GoogleTest finds this by argument-dependent lookup, so a matcher over
// RecordingEventSink::logs prints "Warning" rather than "4-byte object".
inline void PrintTo(LogLevel level, std::ostream *os)
{
    switch (level)
    {
    case LogLevel::kError:
        *os << "Error";
        return;
    case LogLevel::kWarning:
        *os << "Warning";
        return;
    case LogLevel::kInfo:
        *os << "Info";
        return;
    case LogLevel::kDebug:
        *os << "Debug";
        return;
    }
    *os << "LogLevel(" << static_cast<int>(level) << ")";
}

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
    void Log(LogLevel level, std::string_view message) override
    {
        logs.emplace_back(level, std::string{message});
    }
    void Progress(int done, int total) override
    {
        progress_calls.emplace_back(done, total);
    }
    void PhaseProgress(const PhaseProgressEvent& event) override
    {
        IEventSink::PhaseProgress(event);
        phase_progress_calls.push_back(
            {std::string(event.phase_name), event.phase_index, event.phase_count, event.done, event.total});
    }
    void Notice(std::string_view message) override
    {
        notices.emplace_back(message);
    }
    std::vector<std::pair<fastecu::LogLevel, std::string>> logs;
    std::vector<std::pair<int, int>> progress_calls;
    std::vector<RecordedPhaseProgress> phase_progress_calls;
    std::vector<std::string> notices;
};

} // namespace fastecu
