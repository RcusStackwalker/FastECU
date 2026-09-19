#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using namespace std::chrono_literals;
using bytes::Byte;
using bytes::Bytes;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::ManualCancellationToken;
using fastecu::RecordingEventSink;
using fastecu::Status;
using fastecu::flash::build_subaru_tcu_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::FlashPlan;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::ScriptedTransportInitialState;
using fastecu::flash::SubaruTcuHitachiM32rCanExecutor;
using testing::Contains;
using testing::HasSubstr;
using testing::Not;
using testing::Pair;

constexpr std::string_view kProtocol = "sub_tcu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB";

Bytes framed(std::uint32_t request_id, std::initializer_list<Byte> payload)
{
    Bytes result;
    bytes::appendU32Be(result, request_id);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Bytes response(std::initializer_list<Byte> payload)
{
    return framed(0x7E9, payload);
}

FlashPlan readPlan()
{
    auto plan = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

constexpr std::initializer_list<Byte> kAlive{0x71, 0x02, 0x02, 0x03};

void scriptKernelProbeMiss(ScriptedCanFlashTransport& transport)
{
    transport.exchange(framed(0x7E1, {0x31, 0x02, 0x02, 0x01}), response({0x7F, 0x31, 0x22}));
}

void scriptIdentity(ScriptedCanFlashTransport& transport, bool valid_tcu = true, bool valid_cal = true)
{
    transport.exchange(framed(0x7E0, {0xAA}), valid_tcu
                                                  ? response({0xEA, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55})
                                                  : response({0x7F, 0xAA, 0x22}));
    transport.exchange(framed(0x7E0, {0x09, 0x04}),
                       valid_cal ? response({0x49, 0x04, 0x00, 0x43, 0x41, 0x4C}) : response({0x7F, 0x09, 0x22}));
}

void scriptSession(ScriptedCanFlashTransport& transport, bool valid = true)
{
    transport.exchange(framed(0x7E0, {0x10, 0x03}), valid ? response({0x50, 0x03}) : response({0x7F, 0x10, 0x22}));
}

void scriptSeed(ScriptedCanFlashTransport& transport, bool valid = true)
{
    transport.exchange(framed(0x7E0, {0x27, 0x01}), valid ? response({0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF})
                                                          : response({0x7F, 0x27, 0x35, 0xDE, 0xAD, 0xBE}));
}

void scriptKey(ScriptedCanFlashTransport& transport, bool valid = true)
{
    // Hand-checked legacy vector: seed DE AD BE EF -> key 30 3C 73 3A.
    transport.exchange(framed(0x7E0, {0x27, 0x02, 0x30, 0x3C, 0x73, 0x3A}),
                       valid ? response({0x67, 0x02}) : response({0x67, 0x03}));
}

void scriptJump(ScriptedCanFlashTransport& transport, bool valid = true)
{
    transport.exchange(framed(0x7E1, {0x10, 0x02}), valid ? response({0x50, 0x02}) : response({0x7F, 0x10, 0x22}));
}

void scriptRecheck(ScriptedCanFlashTransport& transport, bool valid = true)
{
    // Deliberate divergence 3: this request is eight bytes including its
    // CAN-ID prefix. Legacy wrote bytes 6 and 7 past a six-byte QByteArray.
    transport.exchange(framed(0x7E1, {0x31, 0x02, 0x02, 0x01}),
                       valid ? response(kAlive) : response({0x71, 0x02, 0x02, 0x04}));
}

void scriptFullConnect(ScriptedCanFlashTransport& transport, bool valid_tcu = true, bool valid_cal = true,
                       bool valid_jump = true)
{
    const auto section = transport.section("connect");
    scriptKernelProbeMiss(transport);
    scriptIdentity(transport, valid_tcu, valid_cal);
    scriptSession(transport);
    scriptSeed(transport);
    scriptKey(transport);
    scriptJump(transport, valid_jump);
    scriptRecheck(transport);
}

enum class CallKind
{
    Write,
    Sleep,
    Read,
};

struct Call
{
    CallKind kind;
    std::chrono::milliseconds duration;
    bool operator==(const Call&) const = default;
};

class TracingTransport final : public ScriptedCanFlashTransport
{
  public:
    explicit TracingTransport(std::vector<Call>& trace)
        : ScriptedCanFlashTransport(ScriptedTransportInitialState::Open), trace_(trace)
    {
    }

    Status write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        trace_.push_back({CallKind::Write, 0ms});
        return ScriptedCanFlashTransport::write(data, cancellation);
    }

    fastecu::Result<std::optional<Bytes>> read(std::chrono::milliseconds timeout,
                                               const fastecu::ICancellationToken& cancellation) override
    {
        trace_.push_back({CallKind::Read, timeout});
        return ScriptedCanFlashTransport::read(timeout, cancellation);
    }

  private:
    std::vector<Call>& trace_;
};

class TracingClock final : public FakeClock
{
  public:
    explicit TracingClock(std::vector<Call>& trace) : trace_(trace)
    {
    }

    Status sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken& cancellation) override
    {
        trace_.push_back({CallKind::Sleep, duration});
        return FakeClock::sleep(duration, cancellation);
    }

  private:
    std::vector<Call>& trace_;
};

fastecu::Result<fastecu::flash::FlashExecutionResult> execute(SubaruTcuHitachiM32rCanExecutor& executor, FlashPlan plan,
                                                              ScriptedCanFlashTransport& transport, FakeClock& clock,
                                                              RecordingEventSink& events)
{
    ManualCancellationToken cancellation;
    return executor.execute(plan, transport, clock, cancellation, events);
}

TEST(SubaruTcuHitachiM32rCanExecutor, TransportSetupMatchesLegacyCanConfiguration)
{
    SubaruTcuHitachiM32rCanExecutor executor;
    const auto config = executor.transport_setup(readPlan());

    ASSERT_THAT(config, fastecu::testing::IsOk());
    EXPECT_EQ(config->request_id, 0x7E1U);
    EXPECT_EQ(config->response_id, 0x7E9U);
    EXPECT_EQ(config->bitrate, 500000);
    EXPECT_FALSE(config->extended_id);
}

TEST(SubaruTcuHitachiM32rCanExecutor, KernelAlreadyRunningShortCircuitsAfterFirstFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    transport.exchange(framed(0x7E1, {0x31, 0x02, 0x02, 0x01}), response(kAlive));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_EQ(transport.writesConsumed(), 1U);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.readTimeouts(), std::vector{2000ms});
    EXPECT_EQ(clock.elapsed(), 0ms);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FullConnectPreservesFrameOrderPacingAndTimeouts)
{
    std::vector<Call> trace;
    TracingTransport transport{trace};
    scriptFullConnect(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    TracingClock clock{trace};
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 8U);
    EXPECT_EQ(transport.readTimeouts(), std::vector(8, 2000ms));
    EXPECT_EQ(clock.elapsed(), 450ms);
    const std::vector<Call> expected{
        {CallKind::Write, 0ms},   {CallKind::Read, 2000ms}, {CallKind::Write, 0ms},   {CallKind::Sleep, 50ms},
        {CallKind::Read, 2000ms}, {CallKind::Write, 0ms},   {CallKind::Sleep, 50ms},  {CallKind::Read, 2000ms},
        {CallKind::Write, 0ms},   {CallKind::Sleep, 50ms},  {CallKind::Read, 2000ms}, {CallKind::Write, 0ms},
        {CallKind::Sleep, 50ms},  {CallKind::Read, 2000ms}, {CallKind::Write, 0ms},   {CallKind::Sleep, 50ms},
        {CallKind::Read, 2000ms}, {CallKind::Write, 0ms},   {CallKind::Sleep, 200ms}, {CallKind::Read, 2000ms},
        {CallKind::Write, 0ms},   {CallKind::Read, 2000ms},
    };
    EXPECT_EQ(trace, expected);
}

TEST(SubaruTcuHitachiM32rCanExecutor, SuccessfulIdentityResponsesLogDecodedFields)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Info, "TCU ID: 1122334455")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Info, "CAL ID: CAL")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(fastecu::LogLevel::Info, HasSubstr("TCU ID response:")))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(fastecu::LogLevel::Info, HasSubstr("CAL ID response:")))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, ShortIdentityFieldsLogAndContinueSafely)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptKernelProbeMiss(transport);
    transport.exchange(framed(0x7E0, {0xAA}), response({0xEA, 0x00, 0x00}));
    transport.exchange(framed(0x7E0, {0x09, 0x04}), response({0x49, 0x04, 0x00}));
    scriptSession(transport);
    scriptSeed(transport);
    scriptKey(transport);
    scriptJump(transport);
    scriptRecheck(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, "TCU ID response is too short")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, "CAL ID response is too short")));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalTcuIdMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport, false, true, true);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 8U);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for TCU ID"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalCalIdMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport, true, false, true);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 8U);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for CAL ID"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalKernelJumpMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport, true, true, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 8U);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for kernel jump"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalSessionMismatchStopsAtStepFour)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptKernelProbeMiss(transport);
    scriptIdentity(transport);
    scriptSession(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 4U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalSeedMismatchStopsAtStepFive)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptKernelProbeMiss(transport);
    scriptIdentity(transport);
    scriptSession(transport);
    scriptSeed(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 5U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalKeyMismatchStopsAtStepSix)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptKernelProbeMiss(transport);
    scriptIdentity(transport);
    scriptSession(transport);
    scriptSeed(transport);
    scriptKey(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 6U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalFinalRecheckMismatchStopsAtStepEight)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptKernelProbeMiss(transport);
    scriptIdentity(transport);
    scriptSession(transport);
    scriptSeed(transport);
    scriptKey(transport);
    scriptJump(transport);
    scriptRecheck(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 8U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, SeedResponseShorterThanTenBytesIsBadResponse)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptKernelProbeMiss(transport);
    scriptIdentity(transport);
    scriptSession(transport);
    transport.exchange(framed(0x7E0, {0x27, 0x01}), response({0x67, 0x01, 0xDE}));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 5U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, KernelAliveRecheckIsAnEightByteFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Internal, HasSubstr("transfer lands")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 8U);
}
} // namespace
