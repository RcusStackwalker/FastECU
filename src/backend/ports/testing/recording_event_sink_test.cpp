#include "src/backend/ports/testing/recording_event_sink.h"
#include <gtest/gtest.h>
#include <gmock/gmock-matchers.h>

TEST(EventSink, RecordsLogProgressNotice)
{
    using ::testing::ElementsAre;
    using ::testing::Pair;
    fastecu::RecordingEventSink s;
    s.log(fastecu::LogLevel::Warning, "car not responding");
    s.progress(3, 10);
    s.phase_progress({"write", 2, 3, 4, 8});
    s.notice("done");
    EXPECT_THAT(s.logs, ElementsAre(Pair(fastecu::LogLevel::Warning, "car not responding")));
    EXPECT_THAT(s.progress_calls, ElementsAre(Pair(3, 10), Pair(4, 8)));
    ASSERT_EQ(s.phase_progress_calls.size(), 1U);
    EXPECT_EQ(s.phase_progress_calls[0].phase_name, "write");
    EXPECT_THAT(s.notices, ElementsAre("done"));
}
