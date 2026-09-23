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
    static Result<bytes::Bytes> read_range(std::uint32_t begin, std::uint32_t end, IKlineFlashTransport& transport,
                                           IClock& clock, const ICancellationToken& cancellation, IEventSink& events)
    {
        return SubaruUnisiaJecsExecutor::read_range(begin, end, transport, clock, cancellation, events);
    }
};

namespace
{
using namespace std::chrono_literals;

class TracingRawTransport final : public ScriptedKlineFlashTransport
{
  public:
    using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;

    Result<std::size_t> write_raw(bytes::ByteView data) override
    {
        trace.push_back('W');
        return ScriptedKlineFlashTransport::write_raw(data);
    }
    Result<OptionalBytes> read_raw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        trace.push_back('R');
        return ScriptedKlineFlashTransport::read_raw(timeout, cancellation);
    }

    std::vector<char> trace;
};

class FaultingTransport final : public ScriptedKlineFlashTransport
{
  public:
    enum class Fault
    {
        None,
        WakeError,
        WakeShort,
        RawWriteError,
        RawWriteShort,
        RawReadError,
    };

    explicit FaultingTransport(Fault fault)
        : ScriptedKlineFlashTransport(ScriptedTransportInitialState::Open), fault_(fault)
    {
    }

    Result<std::size_t> write(bytes::ByteView data) override
    {
        if (fault_ == Fault::WakeError)
        {
            return fail(ErrorKind::Disconnected, "injected wake write failure");
        }
        if (fault_ == Fault::WakeShort)
        {
            return data.size() - 1U;
        }
        return ScriptedKlineFlashTransport::write(data);
    }
    Result<std::size_t> write_raw(bytes::ByteView data) override
    {
        if (fault_ == Fault::RawWriteError)
        {
            return fail(ErrorKind::Disconnected, "injected raw write failure");
        }
        if (fault_ == Fault::RawWriteShort)
        {
            return data.size() - 1U;
        }
        return ScriptedKlineFlashTransport::write_raw(data);
    }
    Result<OptionalBytes> read_raw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (fault_ == Fault::RawReadError)
        {
            return fail(ErrorKind::Timeout, "injected raw read failure");
        }
        return ScriptedKlineFlashTransport::read_raw(timeout, cancellation);
    }

  private:
    Fault fault_;
};

void expect_raw(ScriptedKlineFlashTransport& transport, std::initializer_list<bytes::Byte> values)
{
    transport.expectRawWrite(bytes::Bytes(values));
}

void queue_raw(ScriptedKlineFlashTransport& transport, std::initializer_list<bytes::Byte> values)
{
    transport.queueRawRead(bytes::Bytes(values));
}

FlashPlan read_plan()
{
    auto plan =
        build_subaru_unisia_jecs_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_m3779x", "M3779x", std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

void script_wakeup(ScriptedKlineFlashTransport& transport)
{
    transport.exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
}

Result<FlashExecutionResult> execute_read(ScriptedKlineFlashTransport& transport, FakeClock& clock,
                                          const ICancellationToken& cancellation, RecordingEventSink& events)
{
    return SubaruUnisiaJecsExecutor{}.execute(read_plan(), transport, clock, cancellation, events);
}

TEST(SubaruUnisiaJecsExecutor, TransportSetupRequests1953BaudEvenParity)
{
    const auto setup = SubaruUnisiaJecsExecutor{}.transport_setup(read_plan());
    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 1953);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0);
    EXPECT_EQ(setup->target_id, 0);
    EXPECT_EQ(setup->parity, KlineParity::Even);
}

TEST(SubaruUnisiaJecsExecutor, ReadsEveryAddressThroughRawTransport)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    script_wakeup(transport);
    for (std::uint32_t address = 0; address < 0x10000; ++address)
    {
        expect_raw(transport, {0x78, static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address), 0x00});
        queue_raw(transport, {static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address),
                              static_cast<bytes::Byte>(address ^ (address >> 8U))});
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = execute_read(transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), 0x10000U);
    EXPECT_EQ(result->read_bytes->at(0x00ff), 0xff);
    EXPECT_EQ(result->read_bytes->at(0x0100), 0x01);
    EXPECT_EQ(result->read_bytes->at(0xffff), 0x00);
    EXPECT_EQ(clock.elapsed(), 500ms + 0x10000 * 45ms);
    EXPECT_EQ(events.progress_calls.front(), std::pair(1, 0x10000));
    EXPECT_EQ(events.progress_calls.back(), std::pair(0x10000, 0x10000));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, PreservesSplitAndMultiTupleRawReadsAcrossAddresses)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    expect_raw(transport, {0x78, 0x00, 0x00, 0x00});
    queue_raw(transport, {0x99, 0x00});
    queue_raw(transport, {0x00, 0xaa, 0x00, 0x01, 0xbb});
    expect_raw(transport, {0x78, 0x00, 0x01, 0x00});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes = SubaruUnisiaJecsExecutorTestPeer::read_range(0, 2, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xaa, 0xbb));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, EncodesAddressRolloverBigEndian)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    expect_raw(transport, {0x78, 0x00, 0xff, 0x00});
    queue_raw(transport, {0x00, 0xff, 0xa5});
    expect_raw(transport, {0x78, 0x01, 0x00, 0x00});
    queue_raw(transport, {0x01, 0x00, 0x5a});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes =
        SubaruUnisiaJecsExecutorTestPeer::read_range(0xff, 0x101, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xa5, 0x5a));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, DiscardsWholeWrongTupleAfterSynchronization)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    expect_raw(transport, {0x78, 0x00, 0x00, 0x00});
    queue_raw(transport, {0x00, 0x00, 0xaa});
    expect_raw(transport, {0x78, 0x00, 0x01, 0x00});
    queue_raw(transport, {0x99, 0x00, 0x01, 0x00, 0x01, 0xbb});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes = SubaruUnisiaJecsExecutorTestPeer::read_range(0, 2, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xaa, 0xbb));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, RetransmitsAfterOneHundredEmptyRawReads)
{
    TracingRawTransport transport{ScriptedTransportInitialState::Open};
    expect_raw(transport, {0x78, 0x12, 0x34, 0x00});
    for (int i = 0; i < 100; ++i)
    {
        queue_raw(transport, {});
    }
    expect_raw(transport, {0x78, 0x12, 0x34, 0x00});
    queue_raw(transport, {0x12, 0x34, 0xab});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto bytes =
        SubaruUnisiaJecsExecutorTestPeer::read_range(0x1234, 0x1235, transport, clock, cancellation, events);
    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_THAT(*bytes, ::testing::ElementsAre(0xab));
    std::vector<char> expected{'W'};
    expected.insert(expected.end(), 100, 'R');
    expected.push_back('W');
    expected.push_back('R');
    EXPECT_EQ(transport.trace, expected);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsExecutor, FailureFromWakeWriteStopsBeforeAnyRead)
{
    FaultingTransport transport{FaultingTransport::Fault::WakeError};
    transport.exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = execute_read(transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, ShortWakeWriteIsDisconnected)
{
    FaultingTransport transport{FaultingTransport::Fault::WakeShort};
    transport.exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(execute_read(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, FailureFromWakeFlushReadPropagates)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    transport.expectWrite(bytes::Bytes{0x78, 0x12, 0x34, 0x00});
    transport.queue_error(ErrorKind::Timeout, "injected wake flush failure");
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(execute_read(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, RawWriteFailuresStopBeforeRawRead)
{
    for (const auto fault : {FaultingTransport::Fault::RawWriteError, FaultingTransport::Fault::RawWriteShort})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        FaultingTransport transport{fault};
        script_wakeup(transport);
        expect_raw(transport, {0x78, 0x00, 0x00, 0x00});
        queue_raw(transport, {0x00, 0x00, 0xaa});
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(execute_read(transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::Disconnected));
        EXPECT_TRUE(events.progress_calls.empty());
    }
}

TEST(SubaruUnisiaJecsExecutor, FailureFromRawReadPropagates)
{
    FaultingTransport transport{FaultingTransport::Fault::RawReadError};
    script_wakeup(transport);
    expect_raw(transport, {0x78, 0x00, 0x00, 0x00});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(execute_read(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationDuringWakeDelayStopsBeforeFlushRead)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    transport.exchange(bytes::Bytes{0x78, 0x12, 0x34, 0x00}, bytes::Bytes{0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(2);
    RecordingEventSink events;
    EXPECT_THAT(execute_read(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 1U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationDuringAddressDelayStopsBeforeRawRead)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    script_wakeup(transport);
    expect_raw(transport, {0x78, 0x00, 0x00, 0x00});
    queue_raw(transport, {0x00, 0x00, 0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(5);
    RecordingEventSink events;
    EXPECT_THAT(execute_read(transport, clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationBeforeRetryPreventsSecondWrite)
{
    TracingRawTransport transport{ScriptedTransportInitialState::Open};
    expect_raw(transport, {0x78, 0x12, 0x34, 0x00});
    for (int i = 0; i < 100; ++i)
    {
        queue_raw(transport, {});
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    cancellation.set_predicate([&transport]
                               { return static_cast<std::size_t>(std::ranges::count(transport.trace, 'R')) >= 100U; });
    RecordingEventSink events;
    EXPECT_THAT(SubaruUnisiaJecsExecutorTestPeer::read_range(0x1234, 0x1235, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(std::ranges::count(transport.trace, 'W'), 1);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruUnisiaJecsExecutor, CancellationAfterOneBytePreventsNextAddressWrite)
{
    TracingRawTransport transport{ScriptedTransportInitialState::Open};
    expect_raw(transport, {0x78, 0x00, 0x00, 0x00});
    queue_raw(transport, {0x00, 0x00, 0xaa});
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    cancellation.set_predicate([&events] { return !events.progress_calls.empty(); });
    EXPECT_THAT(SubaruUnisiaJecsExecutorTestPeer::read_range(0, 2, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(std::ranges::count(transport.trace, 'W'), 1);
    ASSERT_EQ(events.progress_calls.size(), 1U);
    EXPECT_EQ(events.progress_calls.front(), std::pair(1, 2));
}
} // namespace
} // namespace fastecu::flash
