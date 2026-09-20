#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
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
using fastecu::flash::build_subaru_tcu_hitachi_m32r_kline_plan;
using fastecu::flash::FlashFamily;
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

// Deliberate divergence 2: the window the corrected clamp asks for. Legacy
// read_mem underflowed to 0xFFF00000 and asked for 0x80000 bytes.
constexpr std::uint32_t kRegionStart = 0x8000;
constexpr std::uint32_t kRegionLength = 0x78000;
constexpr std::uint32_t kPageSize = 0x100;
constexpr std::uint32_t kPageCount = kRegionLength / kPageSize;
constexpr std::size_t kRomSize = 0x80000;
constexpr std::size_t kConnectFrames = 8;

Bytes framedBytes(std::uint32_t request_id, bytes::ByteView payload)
{
    Bytes result;
    bytes::appendU32Be(result, request_id);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Bytes framed(std::uint32_t request_id, std::initializer_list<Byte> payload)
{
    return framedBytes(request_id, bytes::ByteView{payload.begin(), payload.size()});
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

// --- read path -------------------------------------------------------------

Bytes readWindowRequest()
{
    Bytes payload{0x34, 0x04, 0x33};
    bytes::appendU24Be(payload, kRegionStart);
    bytes::appendU24Be(payload, kRegionLength);
    return framedBytes(0x7E1, payload);
}

void scriptReadWindow(ScriptedCanFlashTransport& transport, bool valid = true)
{
    const auto section = transport.section("read window setup");
    transport.exchange(readWindowRequest(), valid ? response({0x74, 0x20, 0x01, 0x04}) : response({0x7F, 0x34, 0x22}));
}

// The connect-only tests inherited from task 2 now fall through into the read
// path. A silent TCU at the window-setup step stops the dump immediately with
// a Timeout that no connect step can produce, so each of those tests keeps its
// own assertion while additionally pinning the divergence-2 window bytes.
void scriptSilentReadWindow(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("read window setup (silent)");
    transport.expectWrite(readWindowRequest());
    transport.queue_no_frame();
}

Byte pageByte(std::uint32_t page, std::uint32_t index)
{
    return static_cast<Byte>(((page * 31U) + (index * 17U) + 5U) & 0xFFU);
}

Bytes pagePayload(std::uint32_t page)
{
    Bytes payload(kPageSize);
    for (std::uint32_t index = 0; index < kPageSize; ++index)
    {
        payload[index] = pageByte(page, index);
    }
    return payload;
}

Bytes pageRequest(std::uint32_t page)
{
    Bytes payload{0xB7};
    bytes::appendU24Be(payload, kRegionStart + (page * kPageSize));
    return framedBytes(0x7E1, payload);
}

Bytes pageResponse(std::uint32_t page)
{
    Bytes payload{0xF7};
    const Bytes data = pagePayload(page);
    payload.insert(payload.end(), data.begin(), data.end());
    return framedBytes(0x7E9, payload);
}

void scriptDumpLoop(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("dump loop");
    for (std::uint32_t page = 0; page < kPageCount; ++page)
    {
        transport.exchange(pageRequest(page), pageResponse(page));
    }
}

Bytes stopRequest()
{
    return framed(0x7E1, {0x37});
}

void scriptStop(ScriptedCanFlashTransport& transport, bool valid = true)
{
    const auto section = transport.section("dump stop");
    transport.exchange(stopRequest(), valid ? response({0x77}) : response({0x7F, 0x37, 0x22}));
}

void scriptSilentStop(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("dump stop (silent)");
    transport.expectWrite(stopRequest());
    transport.queue_no_frame();
}

// The image the executor must rebuild, assembled here from the scripted page
// bytes with the tables spelled out rather than borrowed from production, so a
// swapped table or a shifted header strip changes this expectation.
Bytes expectedImage()
{
    // Legacy decrypt_payload (operation.cpp:1021) -- encrypt's four words
    // reversed.
    static constexpr std::array<std::uint16_t, 4> kDecryptTable{0x1075, 0x9E51, 0x8BEF, 0x3B61};
    static constexpr std::array<std::uint8_t, 32> kIndexTransformation{
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

    Bytes dumped;
    dumped.reserve(kRegionLength);
    for (std::uint32_t page = 0; page < kPageCount; ++page)
    {
        const Bytes data = pagePayload(page);
        dumped.insert(dumped.end(), data.begin(), data.end());
    }
    // Deliberate divergence 4: a sized zero buffer. Legacy filled an empty
    // QByteArray through padBytes[i] for i in 0..0x7FFF.
    Bytes image(kRegionStart, Byte{0x00});
    const Bytes decrypted = SsmProtocol::calculatePayload(dumped, static_cast<std::uint32_t>(dumped.size()),
                                                          kDecryptTable, kIndexTransformation);
    image.insert(image.end(), decrypted.begin(), decrypted.end());
    return image;
}

void expectBytesEqual(bytes::ByteView actual, bytes::ByteView expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    const auto [first, second] = std::ranges::mismatch(actual, expected);
    if (first != actual.end())
    {
        const auto offset = static_cast<std::size_t>(first - actual.begin());
        FAIL() << std::format("image differs at offset 0x{:X}: actual 0x{:02X}, expected 0x{:02X}", offset, *first,
                              *second);
    }
}

Bytes slice(bytes::ByteView image, std::size_t offset, std::size_t length)
{
    const bytes::ByteView window = image.subspan(offset, length);
    return Bytes(window.begin(), window.end());
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

constexpr Call kWriteCall{CallKind::Write, 0ms};
constexpr Call kReadCall{CallKind::Read, 2000ms};
// Legacy read_mem:553 -- a 1 ms pause after every dumped page.
constexpr Call kPagePauseCall{CallKind::Sleep, 1ms};

std::string_view describe(CallKind kind)
{
    switch (kind)
    {
    case CallKind::Write:
        return "Write";
    case CallKind::Sleep:
        return "Sleep";
    case CallKind::Read:
        return "Read";
    }
    return "?";
}

void expectTraceEquals(const std::vector<Call>& actual, const std::vector<Call>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    const auto [first, second] = std::ranges::mismatch(actual, expected);
    if (first != actual.end())
    {
        const auto index = static_cast<std::size_t>(first - actual.begin());
        FAIL() << std::format("trace differs at index {}: actual {} {}ms, expected {} {}ms", index,
                              describe(first->kind), first->duration.count(), describe(second->kind),
                              second->duration.count());
    }
}

// The eight connect frames task 2 pinned: no gap before the first or last
// read, 50 ms before steps 2-6, 200 ms before step 7, 2000 ms on every read.
std::vector<Call> connectTrace()
{
    return {
        kWriteCall,
        kReadCall,
        kWriteCall,
        {CallKind::Sleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::Sleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::Sleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::Sleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::Sleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::Sleep, 200ms},
        kReadCall,
        kWriteCall,
        kReadCall,
    };
}

std::vector<Call> fullReadTrace()
{
    std::vector<Call> trace = connectTrace();
    trace.push_back(kWriteCall); // 0x34 window setup, no gap before its read
    trace.push_back(kReadCall);
    for (std::uint32_t page = 0; page < kPageCount; ++page)
    {
        trace.push_back(kWriteCall); // 0xB7 page request
        trace.push_back(kReadCall);
        trace.push_back(kPagePauseCall);
    }
    trace.push_back(kWriteCall); // 0x37 stop
    trace.push_back(kReadCall);
    return trace;
}

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
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_EQ(transport.writesConsumed(), 2U);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.readTimeouts(), std::vector(2, 2000ms));
    EXPECT_EQ(clock.elapsed(), 0ms);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FullConnectPreservesFrameOrderPacingAndTimeouts)
{
    std::vector<Call> trace;
    TracingTransport transport{trace};
    scriptFullConnect(transport);
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    TracingClock clock{trace};
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
    EXPECT_EQ(transport.readTimeouts(), std::vector(kConnectFrames + 1, 2000ms));
    EXPECT_EQ(clock.elapsed(), 450ms);
    std::vector<Call> expected = connectTrace();
    expected.push_back(kWriteCall);
    expected.push_back(kReadCall);
    expectTraceEquals(trace, expected);
}

TEST(SubaruTcuHitachiM32rCanExecutor, SuccessfulIdentityResponsesLogDecodedFields)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
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
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, "TCU ID response is too short")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, "CAL ID response is too short")));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalTcuIdMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport, false, true, true);
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for TCU ID"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalCalIdMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport, true, false, true);
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for CAL ID"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalKernelJumpMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport, true, true, false);
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
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
    scriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
}

// --- read path -------------------------------------------------------------

TEST(SubaruTcuHitachiM32rCanExecutor, ReadWindowSetupTargetsTheClampedRegion)
{
    // Divergence 2: the request bytes are 34 04 33 00 80 00 07 80 00 -- the
    // scripted transport rejects any other window, including the legacy
    // 0xFFF00000/0x80000 pair that its own underflow produced.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
    EXPECT_EQ(readWindowRequest(),
              (Bytes{0x00, 0x00, 0x07, 0xE1, 0x34, 0x04, 0x33, 0x00, 0x80, 0x00, 0x07, 0x80, 0x00}));
}

TEST(SubaruTcuHitachiM32rCanExecutor, SuccessfulReadRebuildsTheFullRomImage)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    scriptDumpLoop(transport);
    scriptStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    // 8 connect frames + window setup + 1920 pages + stop.
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + kPageCount + 1);
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kRomSize);

    const Bytes expected = expectedImage();
    // Divergence 4: the first 0x8000 bytes are a sized zero buffer.
    EXPECT_EQ(std::ranges::count(bytes::ByteView{*result->read_bytes}.subspan(0, kRegionStart), Byte{0x00}),
              static_cast<std::ptrdiff_t>(kRegionStart));
    // The first and last dumped pages, reconstructed: a shifted header strip
    // moves these even where the total length happens to survive.
    EXPECT_EQ(slice(*result->read_bytes, kRegionStart, 8), slice(expected, kRegionStart, 8));
    EXPECT_EQ(slice(*result->read_bytes, kRomSize - 8, 8), slice(expected, kRomSize - 8, 8));
    expectBytesEqual(*result->read_bytes, expected);
}

TEST(SubaruTcuHitachiM32rCanExecutor, ReadLoopPacesEveryPageAndKeepsLegacyTimeouts)
{
    std::vector<Call> trace;
    TracingTransport transport{trace};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    scriptDumpLoop(transport);
    scriptStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    TracingClock clock{trace};
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(transport.readTimeouts(), std::vector(kConnectFrames + 1 + kPageCount + 1, 2000ms));
    // 450 ms of connect pacing plus 1 ms after each of the 1920 pages.
    EXPECT_EQ(clock.elapsed(), 450ms + (kPageCount * 1ms));
    expectTraceEquals(trace, fullReadTrace());
}

TEST(SubaruTcuHitachiM32rCanExecutor, PageResponseWithoutTheDumpServiceIdIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    {
        const auto section = transport.section("dump loop");
        transport.exchange(pageRequest(0), pageResponse(0));
        transport.exchange(pageRequest(1), response({0x7F, 0xB7, 0x22}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    // The message discriminates the 0xF7 check from the length guard below it:
    // without it, dropping the 0xF7 check still reaches a BadResponse by way
    // of the length guard and this test would pin nothing.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, Not(HasSubstr("expected 261"))));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 2);
}

TEST(SubaruTcuHitachiM32rCanExecutor, PageResponseOfTheWrongLengthIsFatal)
{
    // Guard beyond legacy: legacy appended whatever followed the five header
    // bytes, so a short page silently shortened the image it handed back.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    {
        const auto section = transport.section("dump loop");
        Bytes truncated = pageResponse(0);
        truncated.pop_back();
        transport.exchange(pageRequest(0), truncated);
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("expected 261")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, StopFrameMismatchIsNotFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    scriptDumpLoop(transport);
    scriptStop(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    expectBytesEqual(*result->read_bytes, expectedImage());
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for dump stop"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, StopFrameSilenceIsNotFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    scriptDumpLoop(transport);
    scriptSilentStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::Error, HasSubstr("No valid response from TCU for dump stop"))));
}

// --- write path ------------------------------------------------------------

constexpr std::uint32_t kWriteFrameSize = 128;
// Legacy reflash_block retries both the 0x37 close and the 0x31 02 02 01
// checksum up to 20 times (operation.cpp:842, 889).
constexpr int kRetryAttempts = 20;

struct BlockSpec
{
    std::uint32_t start;
    std::uint32_t len;
};

// M32R_512KB blocks 3-10: legacy write_mem's block_modified table marks
// exactly these (operation.cpp:632-633).
constexpr std::array<BlockSpec, 8> kFlashedBlocks{{
    {0x08000, 0x08000},
    {0x10000, 0x10000},
    {0x20000, 0x10000},
    {0x30000, 0x10000},
    {0x40000, 0x10000},
    {0x50000, 0x10000},
    {0x60000, 0x10000},
    {0x70000, 0x10000},
}};

constexpr std::uint32_t kTotalDataFrames = kRegionLength / kWriteFrameSize;

Byte romByte(std::size_t index)
{
    return static_cast<Byte>(((index * 7U) + ((index >> 8U) * 13U) + 3U) & 0xFFU);
}

const Bytes& plaintextRom()
{
    static const Bytes rom = []
    {
        Bytes image(kRomSize);
        for (std::size_t index = 0; index < kRomSize; ++index)
        {
            image[index] = romByte(index);
        }
        return image;
    }();
    return rom;
}

// The encrypted image every 0xB6 frame must carry, built here from the tables
// spelled out rather than borrowed from production, so a swapped or missing
// table changes this expectation.
const Bytes& encryptedRom()
{
    static const Bytes rom = []
    {
        // Legacy encrypt_payload (operation.cpp:1002).
        static constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};
        static constexpr std::array<std::uint8_t, 32> kIndexTransformation{
            0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
            0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
        return SsmProtocol::calculatePayload(plaintextRom(), static_cast<std::uint32_t>(kRomSize), kEncryptTable,
                                             kIndexTransformation);
    }();
    return rom;
}

FlashPlan writePlan()
{
    auto plan = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, plaintextRom());
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

Bytes eraseRequest()
{
    return framed(0x7E1, {0x31, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
}

Bytes blockWindowRequest(const BlockSpec& block)
{
    Bytes payload{0x34, 0x04, 0x33};
    bytes::appendU24Be(payload, block.start);
    bytes::appendU24Be(payload, block.len);
    return framedBytes(0x7E1, payload);
}

Bytes dataRequest(std::uint32_t address)
{
    Bytes payload{0xB6};
    bytes::appendU24Be(payload, address);
    const Bytes& rom = encryptedRom();
    const auto offset = static_cast<std::ptrdiff_t>(address);
    payload.insert(payload.end(), rom.begin() + offset, rom.begin() + offset + kWriteFrameSize);
    return framedBytes(0x7E1, payload);
}

Bytes closeRequest()
{
    return framed(0x7E1, {0x37});
}

Bytes checksumRequest()
{
    return framed(0x7E1, {0x31, 0x02, 0x02, 0x01});
}

// Positive answers carry only the byte(s) legacy actually inspects, so a
// production check widened beyond legacy's would fail here; every negative
// answer below is the same length as its positive twin and differs in exactly
// the byte under test.
void scriptErase(ScriptedCanFlashTransport& transport, std::optional<Bytes> answer = response({0x31, 0x02, 0x01}))
{
    const auto section = transport.section("erase");
    if (answer.has_value())
    {
        transport.exchange(eraseRequest(), *answer);
        return;
    }
    transport.expectWrite(eraseRequest());
    transport.queue_no_frame();
}

void scriptBlockWindow(ScriptedCanFlashTransport& transport, const BlockSpec& block, bool valid = true)
{
    const auto section = transport.section("block window setup");
    transport.exchange(blockWindowRequest(block), valid ? response({0x74}) : response({0x75}));
}

void scriptBlockData(ScriptedCanFlashTransport& transport, const BlockSpec& block)
{
    const auto section = transport.section("block data frames");
    for (std::uint32_t offset = 0; offset < block.len; offset += kWriteFrameSize)
    {
        transport.exchange(dataRequest(block.start + offset), response({0xF6}));
    }
}

void scriptClose(ScriptedCanFlashTransport& transport, bool valid = true)
{
    const auto section = transport.section("block close");
    transport.exchange(closeRequest(), valid ? response({0x77}) : response({0x78}));
}

void scriptChecksum(ScriptedCanFlashTransport& transport, std::optional<Bytes> answer = response({0x71, 0x02, 0x02}))
{
    const auto section = transport.section("block checksum");
    if (answer.has_value())
    {
        transport.exchange(checksumRequest(), *answer);
        return;
    }
    transport.expectWrite(checksumRequest());
    transport.queue_no_frame();
}

void scriptBlock(ScriptedCanFlashTransport& transport, const BlockSpec& block)
{
    scriptBlockWindow(transport, block);
    scriptBlockData(transport, block);
    scriptClose(transport);
    scriptChecksum(transport);
}

void scriptFullWrite(ScriptedCanFlashTransport& transport)
{
    scriptFullConnect(transport);
    scriptErase(transport);
    for (const BlockSpec& block : kFlashedBlocks)
    {
        scriptBlock(transport, block);
    }
}

// 8 connect frames + erase + per block (window + N data + close + checksum).
constexpr std::size_t kFullWriteWrites =
    kConnectFrames + 1 + (kFlashedBlocks.size() * 3) + static_cast<std::size_t>(kTotalDataFrames);

class RecordingTransport final : public ScriptedCanFlashTransport
{
  public:
    RecordingTransport() : ScriptedCanFlashTransport(ScriptedTransportInitialState::Open)
    {
    }

    Status write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        return ScriptedCanFlashTransport::write(data, cancellation);
    }

    std::vector<Bytes> writes;
};

std::uint32_t addressAt(const Bytes& frame, std::size_t offset)
{
    return (static_cast<std::uint32_t>(frame[offset]) << 16U) | (static_cast<std::uint32_t>(frame[offset + 1]) << 8U) |
           static_cast<std::uint32_t>(frame[offset + 2]);
}

// Every address a 0x34 04 33 window-setup frame asked the kernel to open.
std::vector<std::uint32_t> windowStarts(const std::vector<Bytes>& writes)
{
    std::vector<std::uint32_t> starts;
    for (const Bytes& frame : writes)
    {
        if (frame.size() >= 13 && frame[4] == 0x34 && frame[5] == 0x04 && frame[6] == 0x33)
        {
            starts.push_back(addressAt(frame, 7));
        }
    }
    return starts;
}

// Every address a 0xB6 data frame wrote 128 bytes to.
std::vector<std::uint32_t> dataAddresses(const std::vector<Bytes>& writes)
{
    std::vector<std::uint32_t> addresses;
    for (const Bytes& frame : writes)
    {
        if (frame.size() == 8 + kWriteFrameSize && frame[4] == 0xB6)
        {
            addresses.push_back(addressAt(frame, 5));
        }
    }
    return addresses;
}

constexpr Call kEraseSleepCall{CallKind::Sleep, 500ms};
constexpr Call kEraseReadCall{CallKind::Read, 200ms};
constexpr Call kDataSleepCall{CallKind::Sleep, 200ms};
constexpr Call kDataReadCall{CallKind::Read, 500ms};
constexpr Call kLongReadCall{CallKind::Read, 800ms};
constexpr Call kCloseSleepCall{CallKind::Sleep, 100ms};

std::vector<Call> fullWriteTrace()
{
    std::vector<Call> trace = connectTrace();
    trace.push_back(kWriteCall); // erase 31 02 01 FF FF FF FF
    trace.push_back(kEraseSleepCall);
    trace.push_back(kEraseReadCall);
    for (const BlockSpec& block : kFlashedBlocks)
    {
        trace.push_back(kWriteCall); // 0x34 window setup, no gap before its read
        trace.push_back(kReadCall);
        for (std::uint32_t offset = 0; offset < block.len; offset += kWriteFrameSize)
        {
            trace.push_back(kWriteCall); // 0xB6 data frame
            trace.push_back(kDataSleepCall);
            trace.push_back(kDataReadCall);
        }
        trace.push_back(kWriteCall); // 0x37 close
        trace.push_back(kLongReadCall);
        trace.push_back(kCloseSleepCall);
        trace.push_back(kWriteCall); // 0x31 02 02 01 checksum
        trace.push_back(kLongReadCall);
    }
    return trace;
}

std::vector<std::chrono::milliseconds> fullWriteReadTimeouts()
{
    std::vector<std::chrono::milliseconds> timeouts(kConnectFrames, 2000ms);
    timeouts.push_back(200ms);
    for (const BlockSpec& block : kFlashedBlocks)
    {
        timeouts.push_back(2000ms);
        for (std::uint32_t offset = 0; offset < block.len; offset += kWriteFrameSize)
        {
            timeouts.push_back(500ms);
        }
        timeouts.push_back(800ms);
        timeouts.push_back(800ms);
    }
    return timeouts;
}

TEST(SubaruTcuHitachiM32rCanExecutor, DataFramesCarryTheEncryptedImageNotThePlaintext)
{
    // Legacy write_mem encrypts the whole ROM before the first frame leaves
    // (operation.cpp:640). The scripted transport rejects any 0xB6 frame whose
    // 128 payload bytes are not the encrypted ones, and the guard below proves
    // that expectation is not vacuously equal to the plaintext.
    Bytes plaintext_frame{0xB6};
    bytes::appendU24Be(plaintext_frame, kRegionStart);
    plaintext_frame.insert(plaintext_frame.end(), plaintextRom().begin() + kRegionStart,
                           plaintextRom().begin() + kRegionStart + kWriteFrameSize);
    ASSERT_NE(dataRequest(kRegionStart), framedBytes(0x7E1, plaintext_frame));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.section("block data frames");
        transport.exchange(dataRequest(kRegionStart), response({0xF6}));
        transport.exchange(dataRequest(kRegionStart + kWriteFrameSize), response({0xF5}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + 2);
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseRequestMatchesLegacyBytes)
{
    EXPECT_EQ(eraseRequest(), (Bytes{0x00, 0x00, 0x07, 0xE1, 0x31, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0], false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    // The run got past the erase and stopped at the first block window, so the
    // erase bytes and its positive answer are both pinned.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseResponseShorterThanSevenBytesIsFatalBeforeAnyReflashFrame)
{
    // Deliberate divergence 5: legacy erase_mem indexed received.at(4..6) with
    // no length guard and had its `return STATUS_ERROR` commented out, so a
    // failed erase was logged and reflash proceeded onto unerased flash.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport, response({0x31, 0x02}));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("response is too short")));
    EXPECT_TRUE(transport.scriptConsumed());
    // Connect plus the erase itself and nothing more: no window, no 0xB6.
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseContentMismatchIsFatalBeforeAnyReflashFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    // Same length as the positive answer, differing in exactly the last byte.
    scriptErase(transport, response({0x31, 0x02, 0x02}));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, Not(HasSubstr("too short"))));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseSilenceIsFatalBeforeAnyReflashFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport, std::nullopt);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, WriteFlashesBlocksThreeThroughTenAndNothingElse)
{
    RecordingTransport transport;
    scriptFullWrite(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kFullWriteWrites);
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());

    const auto *device = fastecu::flash::find_flash_device(kMcu);
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->numblocks, 11U);

    const std::vector<std::uint32_t> opened = windowStarts(transport.writes);
    std::vector<std::uint32_t> expected_opened;
    for (unsigned blockno = 3; blockno < device->numblocks; ++blockno)
    {
        expected_opened.push_back(device->fblocks[blockno].start);
    }
    EXPECT_EQ(opened, expected_opened);

    // The "not" side, spelled out: blocks 0-2 carry the bootloader. An
    // off-by-one that widened the range downwards would open 0x6000 here, and
    // one that widened it upwards would run off the 11-block table.
    for (unsigned blockno = 0; blockno < 3; ++blockno)
    {
        EXPECT_THAT(opened, Not(Contains(device->fblocks[blockno].start)));
    }
    EXPECT_EQ(opened.size(), kFlashedBlocks.size());

    const std::vector<std::uint32_t> written = dataAddresses(transport.writes);
    ASSERT_EQ(written.size(), kTotalDataFrames);
    EXPECT_EQ(*std::ranges::min_element(written), device->fblocks[3].start);
    EXPECT_EQ(*std::ranges::max_element(written), kRomSize - kWriteFrameSize);
    EXPECT_THAT(written, Not(Contains(0x00000000U)));
    EXPECT_THAT(written, Not(Contains(kRegionStart - kWriteFrameSize)));

    ASSERT_FALSE(events.progress_calls.empty());
    EXPECT_EQ(events.progress_calls.back(),
              std::make_pair(static_cast<int>(kRegionLength), static_cast<int>(kRegionLength)));
}

TEST(SubaruTcuHitachiM32rCanExecutor, WriteLoopPacesEveryFrameAndKeepsLegacyTimeouts)
{
    std::vector<Call> trace;
    TracingTransport transport{trace};
    scriptFullWrite(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    TracingClock clock{trace};
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(transport.readTimeouts(), fullWriteReadTimeouts());
    // 450 ms of connect pacing, 500 ms before the erase read, 200 ms before
    // each of the 3840 data reads, and 100 ms between each close and checksum.
    EXPECT_EQ(clock.elapsed(), 450ms + 500ms + (kTotalDataFrames * 200ms) + (kFlashedBlocks.size() * 100ms));
    expectTraceEquals(trace, fullWriteTrace());
}

TEST(SubaruTcuHitachiM32rCanExecutor, BlockWindowMismatchIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0], false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1);
    EXPECT_EQ(blockWindowRequest(kFlashedBlocks[0]),
              (Bytes{0x00, 0x00, 0x07, 0xE1, 0x34, 0x04, 0x33, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00}));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalCloseAnswerIsRetriedAndTheBlockStillCompletes)
{
    // Legacy treats a non-0x77 close reply as non-fatal (its return is
    // commented out, operation.cpp:861) and simply tries again.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    scriptBlockData(transport, kFlashedBlocks[0]);
    scriptClose(transport, false);
    scriptClose(transport, true);
    scriptChecksum(transport);
    // Block 4 stops the run so the assertions stay on block 3's close.
    scriptBlockWindow(transport, kFlashedBlocks[1], false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    const std::size_t block3_frames = kFlashedBlocks[0].len / kWriteFrameSize;
    // connect + erase + window + data + two closes + checksum + block 4 window.
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + block3_frames + 2 + 1 + 1);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for block close"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, CloseRetriesExhaustAfterTwentyAttemptsAndNoChecksumFollows)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    scriptBlockData(transport, kFlashedBlocks[0]);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        scriptClose(transport, false);
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("did not close after 20")));
    EXPECT_TRUE(transport.scriptConsumed());
    const std::size_t block3_frames = kFlashedBlocks[0].len / kWriteFrameSize;
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + block3_frames + kRetryAttempts);
    // Exhaustion stops before the checksum, so the 100 ms close-to-checksum
    // pause never runs either.
    EXPECT_EQ(clock.elapsed(), 450ms + 500ms + (block3_frames * 200ms));
}

TEST(SubaruTcuHitachiM32rCanExecutor, ChecksumAnswerDifferingInOneByteFailsTheBlock)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    scriptBlockData(transport, kFlashedBlocks[0]);
    scriptClose(transport);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        scriptChecksum(transport, response({0x71, 0x02, 0x03}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("checksum failed after 20 attempts")));
    EXPECT_TRUE(transport.scriptConsumed());
    const std::size_t block3_frames = kFlashedBlocks[0].len / kWriteFrameSize;
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + block3_frames + 1 + kRetryAttempts);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::Error, HasSubstr("Wrong response from TCU for block checksum"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, ChecksumAnswerShorterThanSevenBytesFailsTheBlock)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    scriptBlockData(transport, kFlashedBlocks[0]);
    scriptClose(transport);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        scriptChecksum(transport, response({0x71, 0x02}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("checksum failed after 20 attempts")));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuHitachiM32rCanExecutor, MissingChecksumAnswerFailsTheBlock)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    scriptBlockData(transport, kFlashedBlocks[0]);
    scriptClose(transport);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        scriptChecksum(transport, std::nullopt);
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("checksum failed after 20 attempts")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::Error, HasSubstr("No valid response from TCU for block checksum"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, DataFrameMismatchIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.section("block data frames");
        transport.exchange(dataRequest(kRegionStart), response({0xF6}));
        // Same length as the positive answer, differing in the service id.
        transport.exchange(dataRequest(kRegionStart + kWriteFrameSize), response({0xF5}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::BadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + 2);
}

TEST(SubaruTcuHitachiM32rCanExecutor, DataFrameSilenceIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.section("block data frames");
        transport.expectWrite(dataRequest(kRegionStart));
        transport.queue_no_frame();
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Timeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + 1);
}

// A transport with no script at all: it answers every request positively, so
// nothing stops a widened block range except the assertions in the test below.
// Without it, "blocks 0-2 are not flashed" would rest entirely on the scripted
// transport rejecting the extra frames, and the explicit Not(Contains(...))
// assertions would never run because the IsOk assertion aborts first.
class PermissiveTransport final : public ScriptedCanFlashTransport
{
  public:
    PermissiveTransport() : ScriptedCanFlashTransport(ScriptedTransportInitialState::Open)
    {
    }

    Status write(bytes::ByteView data, const fastecu::ICancellationToken& /*cancellation*/) override
    {
        writes.emplace_back(data.begin(), data.end());
        return {};
    }

    fastecu::Result<std::optional<Bytes>> read(std::chrono::milliseconds /*timeout*/,
                                               const fastecu::ICancellationToken& /*cancellation*/) override
    {
        return std::optional<Bytes>{answerFor(writes.empty() ? Bytes{} : writes.back())};
    }

    std::vector<Bytes> writes;

  private:
    static Bytes answerFor(const Bytes& request)
    {
        if (request.size() < 5)
        {
            return response({0x7F});
        }
        switch (request[4])
        {
        case 0x31:
            // The erase (31 02 01 ...) echoes; every other 0x31 is a kernel
            // liveness or block checksum probe.
            if (request.size() > 6 && request[6] == 0x01)
            {
                return response({0x31, 0x02, 0x01});
            }
            return response({0x71, 0x02, 0x02, 0x03});
        case 0x34:
            return response({0x74});
        case 0xB6:
            return response({0xF6});
        case 0x37:
            return response({0x77});
        default:
            return response({0x7F});
        }
    }
};

TEST(SubaruTcuHitachiM32rCanExecutor, WithNoScriptToStopItTheWriteStillNeverTouchesABootloaderBlock)
{
    PermissiveTransport transport;
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, writePlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());

    const auto *device = fastecu::flash::find_flash_device(kMcu);
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->numblocks, 11U);

    const std::vector<std::uint32_t> opened = windowStarts(transport.writes);
    std::vector<std::uint32_t> expected_opened;
    for (unsigned blockno = 3; blockno < device->numblocks; ++blockno)
    {
        expected_opened.push_back(device->fblocks[blockno].start);
    }
    EXPECT_EQ(opened, expected_opened);
    for (unsigned blockno = 0; blockno < 3; ++blockno)
    {
        EXPECT_THAT(opened, Not(Contains(device->fblocks[blockno].start)));
    }

    const std::vector<std::uint32_t> written = dataAddresses(transport.writes);
    EXPECT_EQ(written.size(), kTotalDataFrames);
    EXPECT_EQ(std::ranges::count_if(written, [](std::uint32_t address) { return address < kRegionStart; }), 0)
        << "a 0xB6 data frame targeted the bootloader region below 0x8000";
    EXPECT_EQ(
        std::ranges::count_if(written, [](std::uint32_t address) { return address + kWriteFrameSize > kRomSize; }), 0)
        << "a 0xB6 data frame ran past the end of the 0x80000 ROM";
}

// --- cancellation and failure paths -----------------------------------------
//
// Every cancellation test below returns ErrorKind::Cancelled, which by itself
// discriminates nothing: read_rom and reflash_block each poll
// cancellation.cancelled() at several points (top of loop, before a write,
// after a write, after a read, and inside clock.sleep), and every one of
// those checkpoints -- plus the top-level "cancelled before setup" check in
// execute() -- returns the same error kind. What pins a specific checkpoint
// is the exact write/sleep/read trace up to the moment cancellation fires,
// asserted at its full length via expectTraceEquals, together with the
// checkpoint's own message (which does distinguish "cancelled during ROM
// read" / "cancelled during block write" from the generic
// "cancelled before/after write/read" checks inside exchange_optional).
//
// Both trip counts below are derived from kConnectFrames (this family's own
// eight-exchange connect sequence), not copied from any other wave: each
// cancellation test lets exactly kConnectFrames iterations of its loop
// complete, then expects the loop's own top-of-iteration check to catch the
// (kConnectFrames+1)-th.

// Flips a shared ManualCancellationToken the instant the shared trace reaches
// a chosen length, always AFTER delegating to the real FakeClock::sleep --
// so the sleep call that reaches the target length always succeeds normally,
// and the flag only becomes visible to the next cancellation.cancelled()
// poll in production code (read_rom's top-of-loop check, for the tests that
// use this). Flipping before delegating would make that same sleep call
// observe its own trip and fail one iteration too early, with the bare,
// unlabeled Cancelled error FakeClock::sleep produces instead of the
// checkpoint's real message.
class TripAfterTraceLengthClock final : public FakeClock
{
  public:
    TripAfterTraceLengthClock(std::vector<Call>& trace, std::size_t trip_at, ManualCancellationToken& cancellation)
        : trace_(trace), trip_at_(trip_at), cancellation_(cancellation)
    {
    }

    Status sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken& cancellation) override
    {
        const Status result = FakeClock::sleep(duration, cancellation);
        trace_.push_back({CallKind::Sleep, duration});
        if (trace_.size() == trip_at_)
        {
            cancellation_.cancel();
        }
        return result;
    }

  private:
    std::vector<Call>& trace_;
    std::size_t trip_at_;
    ManualCancellationToken& cancellation_;
};

// Flips a shared ManualCancellationToken after a chosen number of progress()
// calls. reflash_block's data-frame loop calls events.progress() exactly
// once per frame, immediately after that frame's exchange has fully
// completed (write, the 200 ms pre-read delay, the read, and the
// "cancelled after read" check all already resolved) and before the loop's
// own top-of-iteration check for the next frame -- so this is the one place
// in the write path where nothing but the loop's own boundary check follows.
class TripAfterProgressCallsEventSink final : public RecordingEventSink
{
  public:
    TripAfterProgressCallsEventSink(std::size_t trip_after_calls, ManualCancellationToken& cancellation)
        : trip_after_calls_(trip_after_calls), cancellation_(cancellation)
    {
    }

    void progress(int done, int total) override
    {
        RecordingEventSink::progress(done, total);
        if (++calls_ == trip_after_calls_)
        {
            cancellation_.cancel();
        }
    }

  private:
    std::size_t trip_after_calls_;
    std::size_t calls_ = 0;
    ManualCancellationToken& cancellation_;
};

// connectTrace() + the window setup + `pages` full pages (write, read, the
// 1 ms per-page pause) -- the same shape fullReadTrace() builds, truncated.
std::vector<Call> readTracePrefix(std::size_t pages)
{
    std::vector<Call> trace = connectTrace();
    trace.push_back(kWriteCall); // 0x34 window setup
    trace.push_back(kReadCall);
    for (std::size_t page = 0; page < pages; ++page)
    {
        trace.push_back(kWriteCall); // 0xB7 page request
        trace.push_back(kReadCall);
        trace.push_back(kPagePauseCall);
    }
    return trace;
}

// connectTrace() + the erase + block 0's window setup + `frames` full data
// frames (write, the 200 ms pre-read delay, the read) -- the same block-loop
// shape fullWriteTrace() builds, truncated before the close/checksum steps.
std::vector<Call> writeTracePrefixThroughFrames(std::size_t frames)
{
    std::vector<Call> trace = connectTrace();
    trace.push_back(kWriteCall); // erase 31 02 01 FF FF FF FF
    trace.push_back(kEraseSleepCall);
    trace.push_back(kEraseReadCall);
    trace.push_back(kWriteCall); // block 0's 0x34 window setup
    trace.push_back(kReadCall);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        trace.push_back(kWriteCall); // 0xB6 data frame
        trace.push_back(kDataSleepCall);
        trace.push_back(kDataReadCall);
    }
    return trace;
}

TEST(SubaruTcuHitachiM32rCanExecutor, CancellationDuringTheReadLoopStopsAtTheNextPageBoundary)
{
    constexpr auto kCancelAfterPages = static_cast<std::uint32_t>(kConnectFrames);
    std::vector<Call> trace;
    TracingTransport transport{trace};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    {
        const auto section = transport.section("dump loop");
        for (std::uint32_t page = 0; page < kCancelAfterPages; ++page)
        {
            transport.exchange(pageRequest(page), pageResponse(page));
        }
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    ManualCancellationToken cancellation;
    const std::vector<Call> expected_prefix = readTracePrefix(kCancelAfterPages);
    TripAfterTraceLengthClock clock{trace, expected_prefix.size(), cancellation};
    RecordingEventSink events;

    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    // "cancelled during ROM read" is read_rom's top-of-loop checkpoint --
    // distinct from the generic "cancelled before/after write/read" messages
    // exchange_optional's own checks would produce if this checkpoint were
    // missing and one of those caught it instead.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Cancelled, HasSubstr("cancelled during ROM read")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + kCancelAfterPages);
    expectTraceEquals(trace, expected_prefix);
}

TEST(SubaruTcuHitachiM32rCanExecutor, CancellationDuringTheWriteLoopStopsAtTheNextFrameBoundary)
{
    constexpr auto kCancelAfterFrames = static_cast<std::uint32_t>(kConnectFrames);
    std::vector<Call> trace;
    TracingTransport transport{trace};
    scriptFullConnect(transport);
    scriptErase(transport);
    scriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.section("block data frames");
        for (std::uint32_t frame = 0; frame < kCancelAfterFrames; ++frame)
        {
            transport.exchange(dataRequest(kFlashedBlocks[0].start + (frame * kWriteFrameSize)), response({0xF6}));
        }
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    ManualCancellationToken cancellation;
    TracingClock clock{trace};
    TripAfterProgressCallsEventSink events{kCancelAfterFrames, cancellation};

    const auto result = executor.execute(writePlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Cancelled, HasSubstr("cancelled during block write")));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 1 + kCancelAfterFrames);
    expectTraceEquals(trace, writeTracePrefixThroughFrames(kCancelAfterFrames));
}

TEST(SubaruTcuHitachiM32rCanExecutor, PlanForAnotherFamilyIsRejectedBeforeTransportSetupOrTheWire)
{
    // The K-Line sibling: identical MCU and ROM geometry, differing only in
    // the family tag and the transport kind -- exactly the shape of plan a
    // missing or misplaced check_family() call would let slip through.
    auto plan = build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_kline", kMcu,
                                                         std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ASSERT_EQ(plan->family(), FlashFamily::SubaruTcuHitachiM32rKline);

    SubaruTcuHitachiM32rCanExecutor executor;
    const auto setup_result = executor.transport_setup(*plan);
    EXPECT_THAT(setup_result,
                fastecu::testing::IsErrWith(ErrorKind::InvalidConfig, HasSubstr("plan family does not match")));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    ManualCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto execute_result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(execute_result,
                fastecu::testing::IsErrWith(ErrorKind::InvalidConfig, HasSubstr("plan family does not match")));
    // An empty script: any write at all would fail with a "ran past the end
    // of the script" Internal error instead, so writesConsumed() == 0 and a
    // consumed (trivially, empty) script together prove the wire was never
    // touched.
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuHitachiM32rCanExecutor, TransportErrorDuringTheReadLoopStopsWithoutFurtherPageRequests)
{
    // A port-level error (adapter disconnect, USB drop, ...) is not an ECU
    // content mismatch: it must propagate immediately with its own kind and
    // message, distinct from PageResponseWithoutTheDumpServiceIdIsFatal's
    // BadResponse and from every cancellation checkpoint's Cancelled.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptFullConnect(transport);
    scriptReadWindow(transport);
    {
        const auto section = transport.section("dump loop");
        transport.exchange(pageRequest(0), pageResponse(0));
        transport.expectWrite(pageRequest(1));
        transport.queue_error(ErrorKind::Disconnected, "adapter gone mid-dump");
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = execute(executor, readPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::Disconnected, HasSubstr("adapter gone mid-dump")));
    EXPECT_TRUE(transport.scriptConsumed());
    // Connect + window + page 0 + page 1's write (whose read is what fails):
    // the error surfaces on the very next exchange and no further page is
    // ever requested.
    EXPECT_EQ(transport.writesConsumed(), kConnectFrames + 1 + 2);
}
} // namespace
