#include "src/backend/ports/testing/recording_event_sink.h"
#include <gtest/gtest.h>
TEST(EventSink, RecordsLogProgressNotice)
{
    using ::testing::ElementsAre;
    using ::testing::Pair;
    fastecu::RecordingEventSink s;
    s.log(fastecu::LogLevel::Warning, "car not responding");
    s.progress(3, 10);
    s.notice("done");
    EXPECT_THAT(s.logs, ElementsAre(Pair(fastecu::LogLevel::Warning, "car not responding")));
    EXPECT_THAT(s.progress_calls, ElementsAre(Pair(3, 10)));
    EXPECT_THAT(s.notices, ElementsAre("done"));
}
