#include "src/backend/flash/ecu/subaru_unisia_jecs_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
class SubaruUnisiaJecsExecutorTestPeer
{
  public:
    static Result<bytes::Bytes> ReadRange(std::uint32_t begin, std::uint32_t end, IKlineFlashTransport& transport,
                                          IClock& clock, const ICancellationToken& cancellation, IEventSink& events)
    {
        return SubaruUnisiaJecsExecutor::ReadRange(begin, end, transport, clock, cancellation, events);
    }
};

namespace
{
using namespace std::chrono_literals;

class TracingRawTransport final : public ScriptedKlineFlashTransport
{
  public:
    using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;

    Result<std::size_t> WriteRaw(bytes::ByteView data) override
    {
        trace.push_back('W');
        return ScriptedKlineFlashTransport::WriteRaw(data);
    }
    Result<OptionalBytes> ReadRaw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        trace.push_back('R');
        return ScriptedKlineFlashTransport::ReadRaw(timeout, cancellation);
    }

    std::vector<char> trace;
};

class FaultingTransport final : public ScriptedKlineFlashTransport
{
  public:
    enum class Fault
    {
        kNone,
        kWakeError,
        kWakeShort,
        kRawWriteError,
        kRawWriteShort,
        kRawReadError,
    };

    explicit FaultingTransport(Fault fault)
        : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen), fault_(fault)
    {
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        if (fault_ == Fault::kWakeError)
        {
            return Fail(ErrorKind::kDisconnected, "injected wake write failure");
        }
        if (fault_ == Fault::kWakeShort)
        {
            return data.size() - 1U;
        }
        return ScriptedKlineFlashTransport::Write(data);
    }
    Result<std::size_t> WriteRaw(bytes::ByteView data) override
    {
        if (fault_ == Fault::kRawWriteError)
        {
            return Fail(ErrorKind::kDisconnected, "injected raw write failure");
        }
        if (fault_ == Fault::kRawWriteShort)
        {
            return data.size() - 1U;
        }
        return ScriptedKlineFlashTransport::WriteRaw(data);
    }
    Result<OptionalBytes> ReadRaw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (fault_ == Fault::kRawReadError)
        {
            return Fail(ErrorKind::kTimeout, "injected raw read failure");
        }
        return ScriptedKlineFlashTransport::ReadRaw(timeout, cancellation);
    }

  private:
    Fault fault_;
};

void ExpectRaw(ScriptedKlineFlashTransport& transport, std::initializer_list<bytes::Byte> values)
{
    transport.ExpectRawWrite(bytes::Bytes(values));
}

void QueueRaw(ScriptedKlineFlashTransport& transport, std::initializer_list<bytes::Byte> values)
{
    transport.QueueRawRead(bytes::Bytes(values));
}

FlashPlan ReadPlan()
{
    auto plan = BuildSubaruUnisiaJecsPlan(FlashOperation::kRead, "sub_ecu_unisia_jecs_m3779x", "M3779x", std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

void ScriptWakeup(ScriptedKlineFlashTransport& transport)
{
    transport.Exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
}

Result<FlashExecutionResult> ExecuteRead(ScriptedKlineFlashTransport& transport, FakeClock& clock,
                                         const ICancellationToken& cancellation, RecordingEventSink& events)
{
    return SubaruUnisiaJecsExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);
}

TEST(SubaruUnisiaJecsExecutor, TransportSetupRequests1953BaudEvenParity)
{
    const auto setup = SubaruUnisiaJecsExecutor{}.TransportSetup(ReadPlan());
    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 1953);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0);
    EXPECT_EQ(setup->target_id, 0);
    EXPECT_EQ(setup->parity, KlineParity::kEven);
}

TEST(SubaruUnisiaJecsExecutor, ReadsEveryAddressThroughRawTransport)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptWakeup(transport);
    for (std::uint32_t address = 0; address < 0x10000; ++address)
    {
        ExpectRaw(transport, {0x78, static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address), 0x00});
        QueueRaw(transport, {static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address),
                             static_cast<bytes::Byte>(address ^ (address >> 8U))});
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = ExecuteRead(transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), 0x10000U);
    EXPECT_EQ(result->read_bytes->at(0x00ff), 0xff);
    EXPECT_EQ(result->read_bytes->at(0x0100), 0x01);
    EXPECT_EQ(result->read_bytes->at(0xffff), 0x00);
    EXPECT_EQ(clock.Elapsed(), 500ms + 0x10000 * 46ms);
    EXPECT_EQ(events.progress_calls.front(), std::pair(1, 0x10000));
    EXPECT_EQ(events.progress_calls.back(), std::pair(0x10000, 0x10000));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, PreservesSplitAndMultiTupleRawReadsAcrossAddresses)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x00, 0x00, 0x00});
    QueueRaw(transport, {0x99, 0x00});
    QueueRaw(transport, {0x00, 0xaa, 0x00, 0x01, 0xbb});
    ExpectRaw(transport, {0x78, 0x00, 0x01, 0x00});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes = SubaruUnisiaJecsExecutorTestPeer::ReadRange(0, 2, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xaa, 0xbb));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, EncodesAddressRolloverBigEndian)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x00, 0xff, 0x00});
    QueueRaw(transport, {0x00, 0xff, 0xa5});
    ExpectRaw(transport, {0x78, 0x01, 0x00, 0x00});
    QueueRaw(transport, {0x01, 0x00, 0x5a});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes = SubaruUnisiaJecsExecutorTestPeer::ReadRange(0xff, 0x101, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xa5, 0x5a));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, DiscardsWholeWrongTupleAfterSynchronization)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x00, 0x00, 0x00});
    QueueRaw(transport, {0x00, 0x00, 0xaa});
    ExpectRaw(transport, {0x78, 0x00, 0x01, 0x00});
    QueueRaw(transport, {0x99, 0x00, 0x01, 0x00, 0x01, 0xbb});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes = SubaruUnisiaJecsExecutorTestPeer::ReadRange(0, 2, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xaa, 0xbb));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, RetransmitsAfterOneHundredEmptyRawReads)
{
    TracingRawTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x12, 0x34, 0x00});
    for (int i = 0; i < 100; ++i)
    {
        QueueRaw(transport, {});
    }
    ExpectRaw(transport, {0x78, 0x12, 0x34, 0x00});
    QueueRaw(transport, {0x12, 0x34, 0xab});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes =
        SubaruUnisiaJecsExecutorTestPeer::ReadRange(0x1234, 0x1235, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xab));
    std::vector<char> expected{'W'};
    expected.insert(expected.end(), 100, 'R');
    expected.push_back('W');
    expected.push_back('R');
    EXPECT_EQ(transport.trace, expected);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, ParsesReplyOnHundredthReadBeforeRetransmitting)
{
    TracingRawTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x12, 0x34, 0x00});
    for (int i = 0; i < 99; ++i)
    {
        QueueRaw(transport, {});
    }
    QueueRaw(transport, {0x12, 0x34, 0xab});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes =
        SubaruUnisiaJecsExecutorTestPeer::ReadRange(0x1234, 0x1235, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xab));
    std::vector<char> expected{'W'};
    expected.insert(expected.end(), 100, 'R');
    EXPECT_EQ(transport.trace, expected);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, FailureFromWakeWriteStopsBeforeAnyRead)
{
    FaultingTransport transport{FaultingTransport::Fault::kWakeError};
    transport.Exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = ExecuteRead(transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, ShortWakeWriteIsDisconnected)
{
    FaultingTransport transport{FaultingTransport::Fault::kWakeShort};
    transport.Exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(ExecuteRead(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, FailureFromWakeFlushReadPropagates)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x78, 0x12, 0x34, 0x00});
    transport.QueueError(ErrorKind::kTimeout, "injected wake flush failure");
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(ExecuteRead(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, RawWriteFailuresStopBeforeRawRead)
{
    for (const auto fault : {FaultingTransport::Fault::kRawWriteError, FaultingTransport::Fault::kRawWriteShort})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        FaultingTransport transport{fault};
        ScriptWakeup(transport);
        ExpectRaw(transport, {0x78, 0x00, 0x00, 0x00});
        QueueRaw(transport, {0x00, 0x00, 0xaa});
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(ExecuteRead(transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kDisconnected));
        EXPECT_TRUE(events.progress_calls.empty());
    }
}

TEST(SubaruUnisiaJecsExecutor, FailureFromRawReadPropagates)
{
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError};
    ScriptWakeup(transport);
    ExpectRaw(transport, {0x78, 0x00, 0x00, 0x00});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(ExecuteRead(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationDuringWakeDelayStopsBeforeFlushRead)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(2);
    RecordingEventSink events;
    EXPECT_THAT(ExecuteRead(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationDuringAddressDelayStopsBeforeRawRead)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptWakeup(transport);
    ExpectRaw(transport, {0x78, 0x00, 0x00, 0x00});
    QueueRaw(transport, {0x00, 0x00, 0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(5);
    RecordingEventSink events;
    EXPECT_THAT(ExecuteRead(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationBeforeRetryPreventsSecondWrite)
{
    TracingRawTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x12, 0x34, 0x00});
    for (int i = 0; i < 100; ++i)
    {
        QueueRaw(transport, {});
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    cancellation.SetPredicate([&transport]
                              { return static_cast<std::size_t>(std::ranges::count(transport.trace, 'R')) >= 100U; });
    RecordingEventSink events;
    EXPECT_THAT(SubaruUnisiaJecsExecutorTestPeer::ReadRange(0x1234, 0x1235, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(std::ranges::count(transport.trace, 'W'), 1);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationAfterOneBytePreventsNextAddressWrite)
{
    TracingRawTransport transport{ScriptedTransportInitialState::kOpen};
    ExpectRaw(transport, {0x78, 0x00, 0x00, 0x00});
    QueueRaw(transport, {0x00, 0x00, 0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    cancellation.SetPredicate([&events] { return !events.progress_calls.empty(); });
    EXPECT_THAT(SubaruUnisiaJecsExecutorTestPeer::ReadRange(0, 2, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(std::ranges::count(transport.trace, 'W'), 1);
    ASSERT_EQ(events.progress_calls.size(), 1U);
    EXPECT_EQ(events.progress_calls.front(), std::pair(1, 2));
}
} // namespace
} // namespace fastecu::flash
