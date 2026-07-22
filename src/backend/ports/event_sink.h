#pragma once
#include <string_view>

namespace fastecu
{

enum class LogLevel
{
    Error,
    Warning,
    Info,
    Debug
};

// Replaces backend Qt signals and every backend QMessageBox. Interactive
// confirmation is modeled where a consumer first needs it (step 5c) as a typed
// request answered by the UI, not as a blocking backend prompt.
class IEventSink
{
  public:
    virtual ~IEventSink() = default;
    virtual void log(LogLevel, std::string_view message) = 0;
    virtual void progress(int done, int total) = 0;
    virtual void notice(std::string_view message) = 0;
};

} // namespace fastecu
