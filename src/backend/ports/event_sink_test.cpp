#include "src/backend/ports/event_sink.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using fastecu::IEventSink;
using fastecu::LogLevel;

namespace
{
class RecordingEventSink : public IEventSink
{
  public:
    void log(LogLevel lvl, std::string_view msg) override
    {
        logs.push_back({lvl, std::string(msg)});
    }
    void progress(int done, int total) override
    {
        last = {done, total};
    }
    void notice(std::string_view msg) override
    {
        notices.emplace_back(msg);
    }

    std::vector<std::pair<LogLevel, std::string>> logs;
    std::pair<int, int> last{-1, -1};
    std::vector<std::string> notices;
};
} // namespace

TEST(EventSink, RecordsLogProgressNotice)
{
    RecordingEventSink s;
    s.log(LogLevel::Warning, "car not responding");
    s.progress(3, 10);
    s.notice("done");
    ASSERT_EQ(s.logs.size(), 1u);
    EXPECT_EQ(s.logs[0].first, LogLevel::Warning);
    EXPECT_EQ(s.logs[0].second, "car not responding");
    EXPECT_EQ(s.last, std::make_pair(3, 10));
    ASSERT_EQ(s.notices.size(), 1u);
    EXPECT_EQ(s.notices[0], "done");
}
