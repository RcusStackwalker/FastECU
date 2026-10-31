#pragma once

#include "src/algorithms/protocol/testing/byte_matchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/execute_with_cancellation_guard.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::Optional;

namespace fastecu::flash::testing
{

// The OBK probe miss, the four non-fatal identity queries, the access-method
// probe and the branch selector shared by the Subaru Denso CAN families. Byte 7
// of the raw 0x22 0x10 0x1D reply frame -- payload index 3 -- selects the
// programming branch. `probe_case` 1 leaves the OBK probe unanswered and 2
// answers it with an empty reply. `uds_ecu_id` asks for the ECU id with
// ReadDataByIdentifier 0xF182 instead of the vendor 0xAA/0xEA pair.
inline void ScriptDensoPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branch_byte, int probe_case = 0,
                                     bool uds_ecu_id = false)
{
    const auto section = t.Section("preliminaries");
    t.Exchange(Request({0x10, 0x5F}));
    if (probe_case == 1)
    {
        t.QueueNoFrame();
    }
    else if (probe_case == 2)
    {
        t.QueueRead(Response({}));
    }
    else
    {
        t.QueueRead(Response({0x50, 0x01}));
    }
    if (uds_ecu_id)
    {
        t.Exchange(Request({0x22, 0xF1, 0x82}), Response({0x62, 0xF1, 0x82, 'I', 'D'})); // ECU ID
    }
    else
    {
        t.Exchange(Request({0xAA}), Response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5})); // ECU ID
    }
    t.Exchange(Request({0x09, 0x02}), Response({0x49, 0x02, 'V', 'I', 'N'}));           // VIN
    t.Exchange(Request({0x09, 0x04}), Response({0x49, 0x04, 'C', 'A', 'L'}));           // CAL ID
    t.Exchange(Request({0x09, 0x06}), Response({0x49, 0x06, 0xAA, 0xBB}));              // CVN
    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));                          // access method
    t.Exchange(Request({0x22, 0x10, 0x1D}), Response({0x62, 0x10, 0x1D, branch_byte})); // branch selector
}

// A plan built field by field rather than by the family's plan builder, so a
// plan whose operation the builder itself would refuse can still reach the
// executor -- the only way to prove the executor's own plan validation rejects
// it before any I/O.
inline Result<FlashPlan> HandBuiltCanPlan(FlashOperation operation, FlashFamily family, std::string_view protocol,
                                          std::string_view mcu, const MemoryRegion& block, std::size_t image_size,
                                          FamilyPlan family_plan)
{
    FlashPlanFields fields;
    fields.operation = operation;
    fields.family = family;
    fields.transport = TransportKind::kCanIso15765;
    fields.target_id = std::string(protocol);
    fields.mcu_name = std::string(mcu);
    fields.transfer_region = block;
    fields.erase_regions = {block};
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = std::move(family_plan);
    return ValidateAndBuild(std::move(fields));
}

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
        : result(
              [&]
              {
                  Executor executor;
                  return ExecuteWithCancellationGuard(executor, plan, transport, clock, cancellation, events);
              }())
    {
    }
};

// The seed and key exchanges both read with the shared two-second timeout.
inline void ExpectSeedAndKeyReadTimeouts(const ScriptedCanFlashTransport& transport)
{
    ASSERT_GT(transport.ReadTimeouts().size(), 9U);
    EXPECT_EQ(transport.ReadTimeouts()[8], std::chrono::milliseconds(2000));
    EXPECT_EQ(transport.ReadTimeouts()[9], std::chrono::milliseconds(2000));
}

// Runs a scripted read that must succeed and checks the padded image it
// returns: `lead_pad` bytes of 0xFF, the block filled with `fill`, then a
// 0x100-byte 0xFF tail. `inspect(run, transport)` adds the caller's own checks.
struct NoInspection
{
    template <typename Run, typename Transport> void operator()(const Run&, const Transport&) const
    {
    }
};

template <typename Executor, typename Inspect = NoInspection>
void ExpectPaddedRead(const Result<FlashPlan>& plan, ScriptedCanFlashTransport& transport, std::size_t image_size,
                      std::size_t lead_pad, std::size_t block_length, bytes::Byte fill, Inspect&& inspect = {})
{
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto run = ExecutorRun<Executor>(*plan, transport);
    ASSERT_THAT(run.result, fastecu::testing::IsOk());
    ASSERT_THAT(run.result->read_bytes, Optional(_));
    const bytes::Bytes& rom = *run.result->read_bytes;
    ASSERT_EQ(rom.size(), image_size);
    EXPECT_THAT(bytes::ByteView(rom).first(lead_pad), test_bytes::BytesFilledWith(0xFF));
    EXPECT_THAT(bytes::ByteView(rom).subspan(lead_pad, block_length), test_bytes::BytesFilledWith(fill));
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), test_bytes::BytesFilledWith(0xFF));
    EXPECT_TRUE(transport.ScriptConsumed());
    inspect(run, transport);
}

// Runs a read of `plan` against the scripted `transport` and expects it to fail
// with `kind` having consumed the whole script.
template <typename Executor>
void ExpectReadFailsWith(const Result<FlashPlan>& plan, ScriptedCanFlashTransport& transport, ErrorKind kind)
{
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto run = ExecutorRun<Executor>(*plan, transport);

    EXPECT_THAT(run.result, fastecu::testing::IsErr(kind));
    EXPECT_TRUE(transport.ScriptConsumed());
}

// What differs between families in the short-page read test. Page replies of
// 4 bytes (instead of the requested size) must advance the address by the
// requested page size and still return the padded image.
struct ShortPageReadExpectation
{
    std::size_t block_length;
    std::size_t page_size;
    std::size_t lead_pad;
    std::string_view obk_active_log;
    std::vector<std::chrono::milliseconds> sleeps;
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
    ASSERT_THAT(result->read_bytes, Optional(_));
    const std::size_t pages = expect.block_length / expect.page_size;
    ASSERT_EQ(result->read_bytes->size(), expect.lead_pad + pages * 4 + 0x100);
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(expect.lead_pad), test_bytes::BytesFilledWith(0xFF));
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(0x100), test_bytes::BytesFilledWith(0xFF));
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
    if (!exhausted)
    {
        EXPECT_THAT(events.logs, ::testing::Contains(::testing::Pair(LogLevel::kInfo, "Stop request response: 77 ")));
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

// The close-block 0x37 and the checksum verify. UdsClient absorbs the
// intermediate 0x78 responsePending NRC by re-reading, so only one write is
// expected even though two reads are queued.
inline void ScriptCloseAndChecksum(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("close and checksum");
    t.Exchange(Request({0x37}), Response({0x77}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}), Response({0x7F, 0x31, 0x78}));
    t.QueueRead(Response({0x71, 0x01, 0x02}));
}

// The write image shared by the Denso write tests: every byte a distinct
// function of its offset, so a misplaced page cannot match by accident.
inline bytes::Bytes PatternedRom(std::size_t size)
{
    bytes::Bytes rom(size, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>((i ^ (i >> 8) ^ (i >> 16)) & 0xFF);
    }
    return rom;
}

// A failed identity exchange, even a cancelled one, is logged and stepped over:
// the run goes on to the access-method probe, here scripted to time out.
// `ecu_id_request` is the family's ECU id query and `timeouts` the read
// timeouts it expects, from the OBK probe to the access-method probe.
template <typename Executor, typename MakePlan>
void ExpectIdentityFailuresAreNonFatal(MakePlan make_plan, std::initializer_list<bytes::Byte> ecu_id_request,
                                       const std::vector<std::chrono::milliseconds>& timeouts)
{
    for (const auto error : {ErrorKind::kDisconnected, ErrorKind::kCancelled})
    {
        SCOPED_TRACE(::testing::PrintToString(error));
        ScriptedCanFlashTransport transport;
        transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
        transport.Exchange(Request(ecu_id_request));
        transport.QueueError(error, "identity failure");
        for (const bytes::Byte subfunction : std::to_array<bytes::Byte>({0x02, 0x04, 0x06}))
        {
            transport.Exchange(Request({0x09, subfunction}));
            transport.QueueError(error, "identity failure");
        }
        transport.Exchange(Request({0x10, 0x5F}));
        transport.QueueError(ErrorKind::kTimeout, "stop after identities");
        const auto plan = make_plan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        auto [clock, events, cancellation, result] = ExecutorRun<Executor>(*plan, transport);
        EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(transport.ReadTimeouts(), timeouts);
    }
}

// The scripting members CanExecutorConformance expects of a family's Traits,
// each forwarding to the family's own free helper so the conformance suite
// runs exactly the plans and scripts the local tests do. Derive a Traits from
// it, passing the helpers' addresses (or callables) in this order; a
// BenchConnect taking no probe case is called without one.
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
        if constexpr (std::is_invocable_v<decltype(BenchConnect), ScriptedCanFlashTransport&, int>)
        {
            BenchConnect(t, 0);
        }
        else
        {
            BenchConnect(t);
        }
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
