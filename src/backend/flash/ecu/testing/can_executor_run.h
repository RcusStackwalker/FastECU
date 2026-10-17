#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash::testing
{

// One Execute() of a default-constructed `Executor` against a scripted
// transport, with the collaborators it ran against. Members are public and in
// declaration order so a test can bind them:
//   const auto [clock, events, cancellation, result] = ExecutorRun<E>(*plan, transport);
// The caller asserts the plan built before dereferencing it (see
// can_executor_conformance.h).
template <class Executor, class Clock = FakeClock> struct ExecutorRun
{
    Clock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    Result<FlashExecutionResult> result;

    ExecutorRun(const FlashPlan& plan, ICanFlashTransport& transport)
        : result(Executor{}.Execute(plan, transport, clock, cancellation, events))
    {
    }
};

// What differs between families in the short-page read test. Page replies of
// 4 bytes (instead of the requested size) must advance the address by the
// requested page size and still return the padded image.
enum class StopResponseLog
{
    kLoggedWhenAcknowledged,
    kNeverLogged,
};

struct ShortPageReadExpectation
{
    std::size_t block_length;
    std::size_t page_size;
    std::size_t lead_pad;
    std::string_view obk_active_log;
    std::vector<std::chrono::milliseconds> sleeps;
    StopResponseLog stop_log;
};

// Runs `plan` against the already-scripted `transport` and checks the image,
// progress, logs and timing. The caller asserts the plan is OK and scripts the
// exchanges (whose framing is family-local).
template <class Executor>
void ExpectShortPageRead(const FlashPlan& plan, ScriptedCanFlashTransport& transport, bool exhausted,
                         const ShortPageReadExpectation& expect)
{
    auto [clock, events, cancellation, result] = ExecutorRun<Executor, RecordingClock>(plan, transport);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    const std::size_t pages = expect.block_length / expect.page_size;
    ASSERT_EQ(result->read_bytes->size(), expect.lead_pad + pages * 4 + 0x100);
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(expect.lead_pad), ::testing::Each(0xFF));
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(0x100), ::testing::Each(0xFF));
    for (std::size_t page = 0; page < pages; ++page)
    {
        SCOPED_TRACE(page);
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).subspan(expect.lead_pad + page * 4, 4),
                    ::testing::ElementsAre(0x12, 0x34, 0x56, 0x78));
    }
    std::vector<int> read_progress;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.phase_name == "Read ROM")
        {
            read_progress.push_back(event.done);
        }
    }
    ASSERT_EQ(read_progress.size(), pages + 2);
    EXPECT_EQ(read_progress.front(), 0);
    for (std::size_t page = 1; page <= pages; ++page)
    {
        SCOPED_TRACE(page);
        EXPECT_EQ(read_progress[page],
                  static_cast<int>(page == pages ? expect.block_length - 1 : page * expect.page_size));
    }
    EXPECT_EQ(read_progress.back(), static_cast<int>(expect.block_length));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, ::testing::Contains(::testing::Pair(LogLevel::kInfo, expect.obk_active_log)));
    EXPECT_EQ(clock.sleep_calls, expect.sleeps);
    EXPECT_EQ(transport.ReadTimeouts().back(), std::chrono::milliseconds(500));
    const auto stop_logged = ::testing::Contains(::testing::Pair(LogLevel::kInfo, "Stop request response: 77 "));
    if (expect.stop_log == StopResponseLog::kNeverLogged)
    {
        EXPECT_THAT(events.logs, ::testing::Not(stop_logged));
    }
    else if (!exhausted)
    {
        EXPECT_THAT(events.logs, stop_logged);
    }
}

} // namespace fastecu::flash::testing
