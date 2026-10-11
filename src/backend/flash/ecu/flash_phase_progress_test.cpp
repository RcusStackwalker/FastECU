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

    reporter.Update(3);
    reporter.Update(3);
    reporter.Update(5);

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 3),
                            Field(&RecordedPhaseProgress::done, 5)));
}

TEST(PhaseReporterTest, UpdateClampsToTotalMinusOneSoOnlyCompleteReachesTotal)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.Update(10);

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 9)));
}

TEST(PhaseReporterTest, UpdateNeverMovesDoneBackward)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.Update(6);
    reporter.Update(2);

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 6)));
}

TEST(PhaseReporterTest, CompleteEmitsTotalOnce)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.Complete();
    reporter.Complete();

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 10)));
}

TEST(PhaseReporterTest, CompleteAfterUpdateReachingTotalMinusOneStillEmitsTotal)
{
    RecordingEventSink events;
    PhaseReporter reporter(events, "Write", 1, 1, 10);

    reporter.Update(9);
    reporter.Complete();

    ASSERT_THAT(events.phase_progress_calls,
                ElementsAre(Field(&RecordedPhaseProgress::done, 0), Field(&RecordedPhaseProgress::done, 9),
                            Field(&RecordedPhaseProgress::done, 10)));
}

TEST(PhaseSequenceTest, StartNumbersPhasesInCallOrder)
{
    RecordingEventSink events;
    PhaseSequence phases(events, 3);

    [[maybe_unused]] PhaseReporter first = phases.Start("Connect", 1);
    [[maybe_unused]] PhaseReporter second = phases.Start("Erase", 1);
    [[maybe_unused]] PhaseReporter third = phases.Start("Write", 5);

    ASSERT_EQ(events.phase_progress_calls.size(), 3U);
    EXPECT_EQ(events.phase_progress_calls[0].phase_index, 1);
    EXPECT_EQ(events.phase_progress_calls[1].phase_index, 2);
    EXPECT_EQ(events.phase_progress_calls[2].phase_index, 3);
    EXPECT_EQ(events.phase_progress_calls[0].phase_count, 3);
    EXPECT_EQ(events.phase_progress_calls[2].phase_name, "Write");
    EXPECT_EQ(events.phase_progress_calls[2].total, 5);
}

} // namespace
} // namespace fastecu::flash
