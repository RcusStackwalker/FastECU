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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
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
} // namespace
