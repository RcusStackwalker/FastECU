#pragma once
#include <string_view>

namespace fastecu
{

enum class LogLevel
{
    kError,
    kWarning,
    kInfo,
    kDebug
};

struct PhaseProgressEvent
{
    std::string_view phase_name;
    int phase_index;
    int phase_count;
    int done;
    int total;
};

// Replaces backend Qt signals and every backend QMessageBox. Interactive
// confirmation is modeled where a consumer first needs it (step 5c) as a typed
// request answered by the UI, not as a blocking backend prompt.
class IEventSink
{
  public:
    virtual ~IEventSink() = default;
    virtual void Log(LogLevel, std::string_view message) = 0;
    virtual void Progress(int done, int total) = 0;
    virtual void PhaseProgress(const PhaseProgressEvent& event)
    {
        Progress(event.done, event.total);
    }
    virtual void Notice(std::string_view message) = 0;
};

// A no-op sink for call sites that do not yet have anywhere to route
// progress/log events.
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
