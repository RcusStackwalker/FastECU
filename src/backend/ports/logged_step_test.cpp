#include "src/backend/ports/logged_step.h"

#include <format>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu
{
namespace
{

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using testing::IsErr;
using testing::IsOk;
using testing::IsOkAnd;
using ::testing::Pair;

TEST(LogThen, LogsMessagesInOrderThenRunsTheStep)
{
    RecordingEventSink events;
    const Result<int> result = Result<int>{1}.and_then(LogThen(events, "first", "second",
                                                               [&]
                                                               {
                                                                   events.Log(LogLevel::kDebug, "step");
                                                                   return Result<int>{2};
                                                               }));
    EXPECT_THAT(result, IsOkAnd(2));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "first"), Pair(LogLevel::kInfo, "second"),
                                         Pair(LogLevel::kDebug, "step")));
}

TEST(LogThen, FollowsAStatus)
{
    RecordingEventSink events;
    const Status result = Status{}.and_then(LogThen(events, std::format("n={}", 3), [] { return Status{}; }));
    EXPECT_THAT(result, IsOk());
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "n=3")));
}

TEST(LogThen, PropagatesTheStepFailure)
{
    RecordingEventSink events;
    const Status result =
        Status{}.and_then(LogThen(events, "trying", [] { return Status{Fail(ErrorKind::kTimeout, "late")}; }));
    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
}

TEST(LogThen, DoesNotRunAfterAFailure)
{
    RecordingEventSink events;
    const Status failed = Fail(ErrorKind::kTimeout, "late");
    EXPECT_THAT(failed.and_then(LogThen(events, "trying", [] { return Status{}; })), IsErr(ErrorKind::kTimeout));
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(LogDone, LogsMessagesAndDropsTheValue)
{
    RecordingEventSink events;
    const Status result = Result<std::string>{"reply"}.transform(LogDone(events, "a", "b"));
    EXPECT_THAT(result, IsOk());
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "a"), Pair(LogLevel::kInfo, "b")));
}

} // namespace
} // namespace fastecu
