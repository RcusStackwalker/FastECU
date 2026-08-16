#include "src/backend/flash/ecu/flash_phase_progress.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{

using ::testing::ElementsAre;
using ::testing::Field;

TEST(PhaseReporterTest, ConstructorEmitsDoneZeroImmediately)
{
    RecordingEventSink events;

    PhaseReporter reporter(events, "Erase", 1, 2, 10);

    ASSERT_THAT(events.phase_progress_calls, ElementsAre(Field(&RecordedPhaseProgress::done, 0)));
    EXPECT_EQ(events.phase_progress_calls[0].phase_name, "Erase");
    EXPECT_EQ(events.phase_progress_calls[0].phase_index, 1);
    EXPECT_EQ(events.phase_progress_calls[0].phase_count, 2);
    EXPECT_EQ(events.phase_progress_calls[0].total, 10);
}

TEST(PhaseReporterTest, UpdateEmitsOnlyWhenTheClampedValueChanges)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.update(3);
    reporter.update(3);
    reporter.update(5);

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 3),
                            Field(&RecordedPhaseProgress::done, 5)));
}

TEST(PhaseReporterTest, UpdateClampsToTotalMinusOneSoOnlyCompleteReachesTotal)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.update(10);

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 9)));
}

TEST(PhaseReporterTest, UpdateNeverMovesDoneBackward)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.update(6);
    reporter.update(2);

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 6)));
}

TEST(PhaseReporterTest, CompleteEmitsTotalOnce)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.complete();
    reporter.complete();

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 10)));
}

TEST(PhaseReporterTest, CompleteAfterUpdateReachingTotalMinusOneStillEmitsTotal)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.update(9);
    reporter.complete();

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 9),
                            Field(&RecordedPhaseProgress::done, 10)));
}

TEST(PhaseSequenceTest, StartNumbersPhasesInCallOrder)
{
    RecordingEventSink events;
    PhaseSequence phases(events, 3);

    [[maybe_unused]] PhaseReporter first = phases.start("Connect", 1);
    [[maybe_unused]] PhaseReporter second = phases.start("Erase", 1);
    [[maybe_unused]] PhaseReporter third = phases.start("Write", 5);

    ASSERT_EQ(events.phase_progress_calls.size(), 3u);
    EXPECT_EQ(events.phase_progress_calls[0].phase_index, 1);
    EXPECT_EQ(events.phase_progress_calls[1].phase_index, 2);
    EXPECT_EQ(events.phase_progress_calls[2].phase_index, 3);
    EXPECT_EQ(events.phase_progress_calls[0].phase_count, 3);
    EXPECT_EQ(events.phase_progress_calls[2].phase_name, "Write");
    EXPECT_EQ(events.phase_progress_calls[2].total, 5);
}

} // namespace
} // namespace fastecu::flash
