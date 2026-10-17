#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
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
template <typename Executor, typename Clock = FakeClock> struct ExecutorRun
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
template <typename Executor>
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

// The checksum leg of the close/verify flow after ScriptCloseRetries. Outcomes:
// 0 and 1 succeed (1 after a 0x78 responsePending re-read), 2 times out, 3
// disconnects, 4 is responsePending then a timeout.
inline void ScriptChecksumOutcome(ScriptedCanFlashTransport& t, int outcome)
{
    t.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}));
    if (outcome == 1)
    {
        t.QueueRead(Response({0x7F, 0x31, 0x78}));
        t.QueueRead(Response({0x71, 0x01, 0x02}));
    }
    else if (outcome == 2)
    {
        t.QueueNoFrame();
    }
    else if (outcome == 3)
    {
        t.QueueError(ErrorKind::kDisconnected, "checksum fails");
    }
    else if (outcome == 4)
    {
        t.QueueRead(Response({0x7F, 0x31, 0x78}));
        t.QueueNoFrame();
    }
    else
    {
        t.QueueRead(Response({0x71, 0x01, 0x02}));
    }
}

// Per-family timing of the close/verify flow.
struct CloseOutcomeExpectation
{
    std::vector<std::chrono::milliseconds> sleeps;
    std::chrono::milliseconds checksum_timeout;
    std::chrono::milliseconds reread_timeout;
};

// Executes `plan` against the scripted `transport` and checks `outcome`
// (see ScriptChecksumOutcome) and the close/checksum timing.
template <typename Executor>
void ExpectCloseOutcome(const FlashPlan& plan, ScriptedCanFlashTransport& transport, int outcome,
                        const CloseOutcomeExpectation& expect)
{
    using std::chrono::milliseconds;
    auto [clock, events, cancellation, result] = ExecutorRun<Executor, RecordingClock>(plan, transport);
    if (outcome < 2)
    {
        EXPECT_THAT(result, fastecu::testing::IsOk());
    }
    else
    {
        EXPECT_THAT(result, fastecu::testing::IsErr(outcome == 3 ? ErrorKind::kDisconnected : ErrorKind::kTimeout));
    }
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(clock.sleep_calls, expect.sleeps);
    const auto& timeouts = transport.ReadTimeouts();
    const bool reread = outcome == 1 || outcome == 4;
    ASSERT_GE(timeouts.size(), 8U);
    const auto checksum_index = static_cast<std::ptrdiff_t>(timeouts.size() - (reread ? 2 : 1));
    EXPECT_EQ(timeouts[static_cast<std::size_t>(checksum_index)], expect.checksum_timeout);
    if (reread)
    {
        EXPECT_EQ(timeouts.back(), expect.reread_timeout);
    }
    EXPECT_THAT(std::vector<milliseconds>(timeouts.begin() + checksum_index - 6, timeouts.begin() + checksum_index),
                ::testing::Each(milliseconds(200)));
}

// Short-page read script: every page reply carries 4 payload bytes plus 3
// surplus ones instead of `pagesize`, then the six 0x37 stop sends. Sends 1-5
// fail in different tolerated ways (even a cancelled exchange); the sixth
// fails when `exhausted`, else is acknowledged. No reply length check is added.
inline void ScriptShortPageDumpAndStopRetries(ScriptedCanFlashTransport& t, bool exhausted, std::uint32_t start,
                                              std::uint32_t length, std::uint32_t pagesize)
{
    auto wire = ToWire(bytes::Bytes{0x12, 0x34, 0x56, 0x78});
    wire.insert(wire.end(), {0xAA, 0xBB, 0xCC});
    auto reply = Response({0xF7});
    reply.insert(reply.end(), wire.begin(), wire.end());
    for (std::uint32_t offset = 0; offset < length; offset += pagesize)
    {
        t.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), start + offset)), reply);
    }
    t.Exchange(Request({0x37}));
    t.QueueNoFrame();
    t.Exchange(Request({0x37}), bytes::Bytes{});
    t.Exchange(Request({0x37}), Response({0x7F, 0x37, 0x22}));
    t.Exchange(Request({0x37}));
    t.QueueError(ErrorKind::kDisconnected, "stop disconnected");
    t.Exchange(Request({0x37}));
    t.QueueError(ErrorKind::kCancelled, "stop cancelled");
    t.Exchange(Request({0x37}));
    if (exhausted)
    {
        t.QueueError(ErrorKind::kTimeout, "stop exhausted");
    }
    else
    {
        t.QueueRead(Response({0x77}));
    }
}

// A cancellation raised by the successful-close log must stop the flow before
// the checksum: the last settle is the 100 ms one and no "Verifying checksum"
// is logged.
template <typename Executor> void ExpectCancelAtCloseSettle(const FlashPlan& plan, ScriptedCanFlashTransport& transport)
{
    using std::chrono_literals::operator""ms;
    ManualCancellationToken cancellation;
    CancelOnCloseSuccessSink events(cancellation);
    RecordingClock clock;
    EXPECT_THAT(Executor{}.Execute(plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    ASSERT_FALSE(clock.sleep_calls.empty());
    EXPECT_EQ(clock.sleep_calls.back(), 100ms);
    EXPECT_THAT(events.logs,
                ::testing::Not(::testing::Contains(::testing::Pair(LogLevel::kInfo, "Verifying checksum"))));
}

// The scripting members CanExecutorConformance expects of a family's Traits,
// each forwarding to the family's own free helper so the conformance suite
// runs exactly the plans and scripts the local tests do. Derive a Traits from
// it, passing the helpers' addresses in this order.
template <auto Read, auto HandBuilt, auto BenchConnect, auto ReadSetup, auto FlashDump, auto StopCommand,
          auto UpToFirstFatalRead>
struct CanScriptForwarders
{
    static Result<FlashPlan> ReadPlan()
    {
        return Read();
    }
    static Result<FlashPlan> HandBuiltPlan(FlashOperation operation)
    {
        return HandBuilt(operation);
    }
    static void ScriptBenchConnect(ScriptedCanFlashTransport& t)
    {
        BenchConnect(t, 0);
    }
    static void ScriptReadSetup(ScriptedCanFlashTransport& t)
    {
        ReadSetup(t);
    }
    static void ScriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length,
                                std::uint32_t pagesize, bytes::Byte fill)
    {
        FlashDump(t, start, length, pagesize, fill);
    }
    static void ScriptStopCommand(ScriptedCanFlashTransport& t)
    {
        StopCommand(t);
    }
    static void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
    {
        UpToFirstFatalRead(t);
    }
};

} // namespace fastecu::flash::testing
