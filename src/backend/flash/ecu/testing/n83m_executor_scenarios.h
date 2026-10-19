#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/testing/can_executor_run.h"
#include "src/backend/flash/ecu/testing/n83m_payloads.h"
#include "src/backend/ports/testing/mock_clock.h"

namespace fastecu::flash::testing
{
inline void ExpectN83mOperations(const ScriptedCanFlashTransport& transport,
                                 const std::vector<ScriptedCanFlashTransport::OperationAttempt>& expected)
{
    EXPECT_EQ(transport.FirstUnexpectedOperation(), std::nullopt);
    EXPECT_THAT(transport.OperationAttempts(), ::testing::ElementsAreArray(expected));
    EXPECT_TRUE(transport.ScriptConsumed());
}

enum class SetupSite
{
    kDownload,
    kUpload
};
enum class ReplyShape
{
    kSuccess,
    kNoFrame,
    kEmptyPdu,
    kTruncatedRaw,
    kMismatch,
    kTransportError
};
enum class TransferOperation
{
    kRead,
    kWrite
};
enum class FaultPosition
{
    kFirst,
    kMiddle,
    kLast
};
struct ReadSuccessCase
{
    bool in_car;
    N83mPattern pattern;
};
struct ReadSetupCase
{
    SetupSite site;
    ReplyShape reply;
};
struct TransferFaultCase
{
    TransferOperation operation;
    FaultPosition position;
};

// Expectations are constructed before execution. The fake retains its separate
// queues; only this pilot gives the order between those queues meaning.
struct N83mScript
{
    using Kind = ScriptedCanFlashTransport::OperationKind;
    ScriptedCanFlashTransport transport;
    std::vector<ScriptedCanFlashTransport::OperationAttempt> expected;

    void ReadOutcome(std::optional<ErrorKind> error = {})
    {
        expected.push_back({Kind::kRead, true, error});
    }
    void Exchange(bytes::ByteView request, bytes::ByteView reply)
    {
        transport.Exchange(request, reply);
        expected.push_back({Kind::kWrite, true, {}});
        ReadOutcome();
    }
    void Failure(bytes::ByteView request, ErrorKind error)
    {
        transport.ExpectWrite(request);
        transport.QueueError(error);
        expected.push_back({Kind::kWrite, true, {}});
        ReadOutcome(error);
    }
    void Pairs(std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            expected.push_back({Kind::kWrite, true, {}});
            ReadOutcome();
        }
    }
    void Check() const
    {
        ExpectN83mOperations(transport, expected);
    }
};

class CancelAtTransferProgressSink final : public RecordingEventSink
{
  public:
    CancelAtTransferProgressSink(ManualCancellationToken& token, std::string_view phase, int completed_bytes)
        : token_(token), phase_(phase), completed_bytes_(completed_bytes)
    {
    }
    void PhaseProgress(const PhaseProgressEvent& event) override
    {
        RecordingEventSink::PhaseProgress(event);
        if (event.phase_name == phase_ && event.done == completed_bytes_)
        {
            triggered_ = true;
            token_.Cancel();
        }
    }
    bool Triggered() const
    {
        return triggered_;
    }

  private:
    ManualCancellationToken& token_;
    std::string phase_;
    int completed_bytes_;
    bool triggered_ = false;
};

template <typename Traits> const N83mPayload& N83mImage(N83mPattern pattern)
{
    static const auto kForward = MakeN83mPayload(Traits::kImageSize, N83mPattern::kForward);
    static const auto kReverse = MakeN83mPayload(Traits::kImageSize, N83mPattern::kReverse);
    return pattern == N83mPattern::kForward ? kForward : kReverse;
}

template <typename Traits> void ScriptN83mConnect(N83mScript& script, bool in_car = false)
{
    if (in_car)
    {
        Traits::ScriptInCarConnect(script.transport);
    }
    else
    {
        Traits::ScriptBenchConnect(script.transport);
    }
    script.Pairs(in_car ? 23 : 11);
    if (!in_car && Traits::kDiscardFirstJumpReply)
    {
        script.ReadOutcome();
    }
}

template <typename Traits> bytes::Bytes N83mSetupRequest(SetupSite site)
{
    return Request(bytes::ComposeBe(bytes::Byte(site == SetupSite::kDownload ? 0x34 : 0x35), bytes::Byte(0x04),
                                    bytes::Byte(0x44), std::uint32_t(0x08FAC000), std::uint32_t(Traits::kBlockLength)));
}

template <typename Traits> void ScriptN83mSetup(N83mScript& script)
{
    script.Exchange(N83mSetupRequest<Traits>(SetupSite::kDownload), Response({0x74, 0x20, 0x01, 0x05}));
    script.Exchange(N83mSetupRequest<Traits>(SetupSite::kUpload), Response({0x75, 0x20, 0x01, 0x01}));
}

template <typename Traits>
void ScriptN83mTransfer(N83mScript& script, TransferOperation operation, N83mPattern pattern, std::size_t count)
{
    const auto& payload = N83mImage<Traits>(pattern);
    for (std::size_t page = 0; page < count; ++page)
    {
        const auto address = static_cast<std::uint32_t>(0x08FAC000 + page * 256);
        const auto wire = bytes::ByteView(payload.wire).subspan(0x10000 + page * 256, 256);
        if (operation == TransferOperation::kRead)
        {
            script.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), address)),
                            RequestTo(0x7E8, bytes::ComposeBe(bytes::Byte(0xF7), wire)));
        }
        else
        {
            script.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB6), address, wire)), Response({0xF6}));
        }
    }
}

using N83mSleepPositions = std::vector<std::pair<std::chrono::milliseconds, std::size_t>>;

inline void RecordN83mSleeps(MockClock& clock, RecordingClock& recording, const ScriptedCanFlashTransport& transport,
                             N83mSleepPositions& sleeps)
{
    ON_CALL(clock, Now()).WillByDefault([&recording] { return recording.Now(); });
    ON_CALL(clock, Sleep)
        .WillByDefault(
            [&recording, &transport, &sleeps](auto duration, const auto& token)
            {
                sleeps.emplace_back(duration, transport.OperationAttempts().size());
                return recording.Sleep(duration, token);
            });
}

template <typename Traits> N83mSleepPositions N83mConnectSleeps(bool in_car)
{
    N83mSleepPositions expected{{std::chrono::milliseconds{500}, 14}};
    if (!in_car && Traits::kDiscardFirstJumpReply)
    {
        expected.emplace_back(std::chrono::milliseconds{50}, 22);
    }
    return expected;
}

template <typename Traits> void ExpectN83mRead(ReadSuccessCase test_case)
{
    N83mScript script;
    ScriptN83mConnect<Traits>(script, test_case.in_car);
    ScriptN83mSetup<Traits>(script);
    ScriptN83mTransfer<Traits>(script, TransferOperation::kRead, test_case.pattern, Traits::kBlockLength / 256);
    script.Exchange(Request({0x37}), Response({0x77}));
    const auto plan = Traits::ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    RecordingClock recording;
    MockClock clock;
    N83mSleepPositions sleeps;
    RecordN83mSleeps(clock, recording, script.transport, sleeps);
    ManualCancellationToken token;
    RecordingEventSink events;
    const auto result = typename Traits::Executor{}.Execute(*plan, script.transport, clock, token, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    bytes::Bytes expected(0x10000, 0xFF);
    const auto& payload = N83mImage<Traits>(test_case.pattern);
    expected.insert(expected.end(), payload.plain.begin() + 0x10000, payload.plain.end() - 0x100);
    expected.insert(expected.end(), 0x100, 0xFF);
    EXPECT_THAT(events.logs, ::testing::Not(::testing::IsEmpty()));
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    EXPECT_THAT(*result->read_bytes, test_bytes::BytesEq(expected));
    script.Check();
    EXPECT_THAT(sleeps, ::testing::ElementsAreArray(N83mConnectSleeps<Traits>(test_case.in_car)));
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.front().done, 0);
    EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(Traits::kBlockLength));
    if (!test_case.in_car)
    {
        ExpectSeedAndKeyReadTimeouts(script.transport);
    }
}

template <typename Traits> void ScriptN83mWritePrefix(N83mScript& script)
{
    ScriptN83mConnect<Traits>(script);
    script.Exchange(N83mSetupRequest<Traits>(SetupSite::kDownload), Response({0x74, 0x20, 0x01, 0x05}));
    script.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}), Response({0x71, 0x01, 0x02}));
}

template <typename Traits> void ExpectN83mWrite(N83mPattern pattern)
{
    using std::chrono::milliseconds;
    N83mScript script;
    ScriptN83mWritePrefix<Traits>(script);
    ScriptN83mTransfer<Traits>(script, TransferOperation::kWrite, pattern, Traits::kBlockLength / 256);
    script.Exchange(Request({0x37}), Response({0x77}));
    script.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}), Response({0x71, 0x01, 0x02}));
    const auto plan = Traits::WritePlan(N83mImage<Traits>(pattern).plain);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    RecordingClock recording;
    MockClock clock;
    N83mSleepPositions sleeps;
    RecordN83mSleeps(clock, recording, script.transport, sleeps);
    ManualCancellationToken token;
    RecordingEventSink events;
    const auto result = typename Traits::Executor{}.Execute(*plan, script.transport, clock, token, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(Traits::kBlockLength));
    script.Check();
    const std::size_t bench_reads = Traits::kDiscardFirstJumpReply ? 1 : 0;
    auto expected = N83mConnectSleeps<Traits>(false);
    expected.emplace_back(milliseconds{500}, 25 + bench_reads);
    expected.emplace_back(milliseconds{100}, 28 + bench_reads + 2 * (Traits::kBlockLength / 256));
    EXPECT_THAT(sleeps, ::testing::ElementsAreArray(expected));
}

inline std::optional<ErrorKind> N83mSetupError(ReplyShape shape, bool strict)
{
    if (shape == ReplyShape::kTransportError)
    {
        return ErrorKind::kDisconnected;
    }
    if (shape == ReplyShape::kTruncatedRaw)
    {
        return ErrorKind::kBadResponse;
    }
    if (!strict || shape == ReplyShape::kSuccess)
    {
        return {};
    }
    return shape == ReplyShape::kNoFrame ? ErrorKind::kTimeout : ErrorKind::kBadResponse;
}

template <typename Traits> void ExpectN83mSetup(ReadSetupCase test_case)
{
    N83mScript script;
    ScriptN83mConnect<Traits>(script);
    if (test_case.site == SetupSite::kUpload)
    {
        script.Exchange(N83mSetupRequest<Traits>(SetupSite::kDownload), Response({0x74, 0x20, 0x01, 0x05}));
    }
    script.transport.ExpectWrite(N83mSetupRequest<Traits>(test_case.site));
    script.expected.push_back({N83mScript::Kind::kWrite, true, {}});
    const auto positive = test_case.site == SetupSite::kDownload ? Response({0x74, 0x20, 0x01, 0x05})
                                                                 : Response({0x75, 0x20, 0x01, 0x01});
    switch (test_case.reply)
    {
    case ReplyShape::kSuccess:
        script.transport.QueueRead(positive);
        break;
    case ReplyShape::kNoFrame:
        script.transport.QueueNoFrame();
        break;
    case ReplyShape::kEmptyPdu:
        script.transport.QueueRead(Response({}));
        break;
    case ReplyShape::kTruncatedRaw:
        script.transport.QueueRead(bytes::Bytes{0, 0, 7});
        break;
    case ReplyShape::kMismatch:
        script.transport.QueueRead(Response({0x7F, 0x34, 0x31}));
        break;
    case ReplyShape::kTransportError:
        script.transport.QueueError(ErrorKind::kDisconnected);
        break;
    }
    script.ReadOutcome(test_case.reply == ReplyShape::kTransportError ? std::optional{ErrorKind::kDisconnected}
                                                                      : std::nullopt);
    const auto error = N83mSetupError(test_case.reply, Traits::kStrictSetup);
    if (!error.has_value())
    {
        if (test_case.site == SetupSite::kDownload)
        {
            script.Exchange(N83mSetupRequest<Traits>(SetupSite::kUpload), Response({0x75, 0x20, 0x01, 0x01}));
        }
        ScriptN83mTransfer<Traits>(script, TransferOperation::kRead, N83mPattern::kForward, Traits::kBlockLength / 256);
        script.Exchange(Request({0x37}), Response({0x77}));
    }
    const auto plan = Traits::ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto run = ExecutorRun<typename Traits::Executor>(*plan, script.transport);
    if (error.has_value())
    {
        EXPECT_THAT(run.result, fastecu::testing::IsErr(*error));
    }
    else
    {
        EXPECT_THAT(run.result, fastecu::testing::IsOk());
    }
    script.Check();
}

template <typename Traits> void ExpectN83mTransferFault(TransferFaultCase test_case)
{
    N83mScript script;
    const bool write = test_case.operation == TransferOperation::kWrite;
    if (write)
    {
        ScriptN83mWritePrefix<Traits>(script);
    }
    else
    {
        ScriptN83mConnect<Traits>(script);
        ScriptN83mSetup<Traits>(script);
    }
    const std::size_t pages = Traits::kBlockLength / 256;
    const std::size_t index = test_case.position == FaultPosition::kFirst    ? 0
                              : test_case.position == FaultPosition::kMiddle ? pages / 2
                                                                             : pages - 1;
    ScriptN83mTransfer<Traits>(script, test_case.operation, N83mPattern::kForward, index);
    if (test_case.position != FaultPosition::kMiddle)
    {
        const auto addr = static_cast<std::uint32_t>(0x08FAC000 + index * 256);
        const auto request =
            write ? Request(bytes::ComposeBe(bytes::Byte(0xB6), addr,
                                             bytes::ByteView(N83mImage<Traits>(N83mPattern::kForward).wire)
                                                 .subspan(0x10000 + index * 256, 256)))
                  : Request(bytes::ComposeBe(bytes::Byte(0xB7), addr));
        if (test_case.position == FaultPosition::kFirst)
        {
            script.Failure(request, ErrorKind::kDisconnected);
        }
        else
        {
            script.Exchange(request, Response({0x7F, bytes::Byte(write ? 0xB6 : 0xB7), 0x31}));
        }
    }
    const auto plan = write ? Traits::WritePlan(N83mImage<Traits>(N83mPattern::kForward).plain) : Traits::ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    FakeClock clock;
    ManualCancellationToken token;
    CancelAtTransferProgressSink events(
        token, write ? "Write ROM" : "Read ROM",
        test_case.position == FaultPosition::kMiddle ? static_cast<int>((pages / 2) * 256) : -1);
    const auto result = typename Traits::Executor{}.Execute(*plan, script.transport, clock, token, events);
    const auto kind = test_case.position == FaultPosition::kFirst    ? ErrorKind::kDisconnected
                      : test_case.position == FaultPosition::kMiddle ? ErrorKind::kCancelled
                                                                     : ErrorKind::kBadResponse;
    EXPECT_THAT(result, fastecu::testing::IsErr(kind));
    EXPECT_EQ(events.Triggered(), test_case.position == FaultPosition::kMiddle);
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.back().phase_name, write ? "Write ROM" : "Read ROM");
    EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(index * 256));
    EXPECT_LT(events.phase_progress_calls.back().done, events.phase_progress_calls.back().total);
    script.Check();
}
} // namespace fastecu::flash::testing
