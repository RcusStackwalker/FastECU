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
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/mock_clock.h"
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
using fastecu::MockClock;
using fastecu::RecordingEventSink;
using fastecu::Status;
using fastecu::flash::BuildSubaruTcuHitachiM32rCanPlan;
using fastecu::flash::BuildSubaruTcuHitachiM32rKlinePlan;
using fastecu::flash::FlashFamily;
using fastecu::flash::FlashOperation;
using fastecu::flash::FlashPlan;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::ScriptedTransportInitialState;
using fastecu::flash::SubaruTcuHitachiM32rCanExecutor;
using testing::Contains;
using testing::DoAll;
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

Bytes FramedBytes(std::uint32_t request_id, bytes::ByteView payload)
{
    Bytes result;
    bytes::AppendU32Be(result, request_id);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Bytes Framed(std::uint32_t request_id, std::initializer_list<Byte> payload)
{
    return FramedBytes(request_id, bytes::ByteView{payload.begin(), payload.size()});
}

Bytes Response(std::initializer_list<Byte> payload)
{
    return Framed(0x7E9, payload);
}

FlashPlan ReadPlan()
{
    auto plan = BuildSubaruTcuHitachiM32rCanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

constexpr std::initializer_list<Byte> kAlive{0x71, 0x02, 0x02, 0x03};

void ScriptKernelProbeMiss(ScriptedCanFlashTransport& transport)
{
    transport.Exchange(Framed(0x7E1, {0x31, 0x02, 0x02, 0x01}), Response({0x7F, 0x31, 0x22}));
}

void ScriptIdentity(ScriptedCanFlashTransport& transport, bool valid_tcu = true, bool valid_cal = true)
{
    transport.Exchange(Framed(0x7E0, {0xAA}), valid_tcu
                                                  ? Response({0xEA, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55})
                                                  : Response({0x7F, 0xAA, 0x22}));
    transport.Exchange(Framed(0x7E0, {0x09, 0x04}),
                       valid_cal ? Response({0x49, 0x04, 0x00, 0x43, 0x41, 0x4C}) : Response({0x7F, 0x09, 0x22}));
}

void ScriptSession(ScriptedCanFlashTransport& transport, bool valid = true)
{
    transport.Exchange(Framed(0x7E0, {0x10, 0x03}), valid ? Response({0x50, 0x03}) : Response({0x7F, 0x10, 0x22}));
}

void ScriptSeed(ScriptedCanFlashTransport& transport, bool valid = true)
{
    transport.Exchange(Framed(0x7E0, {0x27, 0x01}), valid ? Response({0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF})
                                                          : Response({0x7F, 0x27, 0x35, 0xDE, 0xAD, 0xBE}));
}

void ScriptKey(ScriptedCanFlashTransport& transport, bool valid = true)
{
    // Hand-checked legacy vector: seed DE AD BE EF -> key 30 3C 73 3A.
    transport.Exchange(Framed(0x7E0, {0x27, 0x02, 0x30, 0x3C, 0x73, 0x3A}),
                       valid ? Response({0x67, 0x02}) : Response({0x67, 0x03}));
}

void ScriptJump(ScriptedCanFlashTransport& transport, bool valid = true)
{
    transport.Exchange(Framed(0x7E1, {0x10, 0x02}), valid ? Response({0x50, 0x02}) : Response({0x7F, 0x10, 0x22}));
}

void ScriptRecheck(ScriptedCanFlashTransport& transport, bool valid = true)
{
    // Deliberate divergence 3: this request is eight bytes including its
    // CAN-ID prefix. Legacy wrote bytes 6 and 7 past a six-byte QByteArray.
    transport.Exchange(Framed(0x7E1, {0x31, 0x02, 0x02, 0x01}),
                       valid ? Response(kAlive) : Response({0x71, 0x02, 0x02, 0x04}));
}

void ScriptFullConnect(ScriptedCanFlashTransport& transport, bool valid_tcu = true, bool valid_cal = true,
                       bool valid_jump = true)
{
    const auto section = transport.Section("connect");
    ScriptKernelProbeMiss(transport);
    ScriptIdentity(transport, valid_tcu, valid_cal);
    ScriptSession(transport);
    ScriptSeed(transport);
    ScriptKey(transport);
    ScriptJump(transport, valid_jump);
    ScriptRecheck(transport);
}

// --- read path -------------------------------------------------------------

Bytes ReadWindowRequest()
{
    Bytes payload{0x34, 0x04, 0x33};
    bytes::AppendU24Be(payload, kRegionStart);
    bytes::AppendU24Be(payload, kRegionLength);
    return FramedBytes(0x7E1, payload);
}

void ScriptReadWindow(ScriptedCanFlashTransport& transport, bool valid = true)
{
    const auto section = transport.Section("read window setup");
    transport.Exchange(ReadWindowRequest(), valid ? Response({0x74, 0x20, 0x01, 0x04}) : Response({0x7F, 0x34, 0x22}));
}

// T3c: unlike scriptReadWindow's negative case (a 3-byte NRC that trips
// expect_prefix's length guard), this is the full 8-byte frame differing from
// the valid answer at only the last byte -- so only the content compare, not
// the length guard, can reject it.
void ScriptReadWindowWrongContent(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("read window setup (wrong content)");
    transport.Exchange(ReadWindowRequest(), Response({0x74, 0x20, 0x01, 0x03}));
}

// The connect-only tests inherited from task 2 now fall through into the read
// path. A silent TCU at the window-setup step stops the dump immediately with
// a Timeout that no connect step can produce, so each of those tests keeps its
// own assertion while additionally pinning the divergence-2 window bytes.
void ScriptSilentReadWindow(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("read window setup (silent)");
    transport.ExpectWrite(ReadWindowRequest());
    transport.QueueNoFrame();
}

Byte PageByte(std::uint32_t page, std::uint32_t index)
{
    return static_cast<Byte>(((page * 31U) + (index * 17U) + 5U) & 0xFFU);
}

Bytes PagePayload(std::uint32_t page)
{
    Bytes payload(kPageSize);
    for (std::uint32_t index = 0; index < kPageSize; ++index)
    {
        payload[index] = PageByte(page, index);
    }
    return payload;
}

Bytes PageRequest(std::uint32_t page)
{
    Bytes payload{0xB7};
    bytes::AppendU24Be(payload, kRegionStart + (page * kPageSize));
    return FramedBytes(0x7E1, payload);
}

Bytes PageResponse(std::uint32_t page)
{
    Bytes payload{0xF7};
    const Bytes data = PagePayload(page);
    payload.insert(payload.end(), data.begin(), data.end());
    return FramedBytes(0x7E9, payload);
}

void ScriptDumpLoop(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("dump loop");
    for (std::uint32_t page = 0; page < kPageCount; ++page)
    {
        transport.Exchange(PageRequest(page), PageResponse(page));
    }
}

Bytes StopRequest()
{
    return Framed(0x7E1, {0x37});
}

void ScriptStop(ScriptedCanFlashTransport& transport, bool valid = true)
{
    const auto section = transport.Section("dump stop");
    transport.Exchange(StopRequest(), valid ? Response({0x77}) : Response({0x7F, 0x37, 0x22}));
}

void ScriptSilentStop(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("dump stop (silent)");
    transport.ExpectWrite(StopRequest());
    transport.QueueNoFrame();
}

// The image the executor must rebuild, assembled here from the scripted page
// bytes with the tables spelled out rather than borrowed from production, so a
// swapped table or a shifted header strip changes this expectation.
Bytes ExpectedImage()
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
        const Bytes data = PagePayload(page);
        dumped.insert(dumped.end(), data.begin(), data.end());
    }
    // Deliberate divergence 4: a sized zero buffer. Legacy filled an empty
    // QByteArray through padBytes[i] for i in 0..0x7FFF.
    Bytes image(kRegionStart, Byte{0x00});
    const Bytes decrypted = ssm_protocol::CalculatePayload(dumped, static_cast<std::uint32_t>(dumped.size()),
                                                           kDecryptTable, kIndexTransformation);
    image.insert(image.end(), decrypted.begin(), decrypted.end());
    return image;
}

void ExpectBytesEqual(bytes::ByteView actual, bytes::ByteView expected)
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

Bytes Slice(bytes::ByteView image, std::size_t offset, std::size_t length)
{
    const bytes::ByteView window = image.subspan(offset, length);
    return Bytes(window.begin(), window.end());
}

enum class CallKind
{
    kWrite,
    kSleep,
    kRead,
};

struct Call
{
    CallKind kind;
    std::chrono::milliseconds duration;
    bool operator==(const Call&) const = default;
};

constexpr Call kWriteCall{CallKind::kWrite, 0ms};
constexpr Call kReadCall{CallKind::kRead, 2000ms};
// Legacy read_mem:553 -- a 1 ms pause after every dumped page.
constexpr Call kPagePauseCall{CallKind::kSleep, 1ms};

std::string_view Describe(CallKind kind)
{
    switch (kind)
    {
    case CallKind::kWrite:
        return "Write";
    case CallKind::kSleep:
        return "Sleep";
    case CallKind::kRead:
        return "Read";
    }
    return "?";
}

void ExpectTraceEquals(const std::vector<Call>& actual, const std::vector<Call>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    const auto [first, second] = std::ranges::mismatch(actual, expected);
    if (first != actual.end())
    {
        const auto index = static_cast<std::size_t>(first - actual.begin());
        FAIL() << std::format("trace differs at index {}: actual {} {}ms, expected {} {}ms", index,
                              Describe(first->kind), first->duration.count(), Describe(second->kind),
                              second->duration.count());
    }
}

// The eight connect frames task 2 pinned: no gap before the first or last
// read, 50 ms before steps 2-6, 200 ms before step 7, 2000 ms on every read.
std::vector<Call> ConnectTrace()
{
    return {
        kWriteCall,
        kReadCall,
        kWriteCall,
        {CallKind::kSleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::kSleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::kSleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::kSleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::kSleep, 50ms},
        kReadCall,
        kWriteCall,
        {CallKind::kSleep, 200ms},
        kReadCall,
        kWriteCall,
        kReadCall,
    };
}

std::vector<Call> FullReadTrace()
{
    std::vector<Call> trace = ConnectTrace();
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
        : ScriptedCanFlashTransport(ScriptedTransportInitialState::kOpen), trace_(trace)
    {
    }

    Status Write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        trace_.push_back({CallKind::kWrite, 0ms});
        return ScriptedCanFlashTransport::Write(data, cancellation);
    }

    fastecu::Result<std::optional<Bytes>> Read(std::chrono::milliseconds timeout,
                                               const fastecu::ICancellationToken& cancellation) override
    {
        trace_.push_back({CallKind::kRead, timeout});
        return ScriptedCanFlashTransport::Read(timeout, cancellation);
    }

  private:
    std::vector<Call>& trace_;
};

// Every sleep appends {Sleep, duration} to the shared trace, then advances time.
void TraceSleeps(MockClock& clock, std::vector<Call>& trace)
{
    ON_CALL(clock, Sleep)
        .WillByDefault(DoAll([&trace](std::chrono::milliseconds duration, const fastecu::ICancellationToken&)
                             { trace.push_back({CallKind::kSleep, duration}); }, clock.SleepOnFake()));
}

fastecu::Result<fastecu::flash::FlashExecutionResult> Execute(SubaruTcuHitachiM32rCanExecutor& executor, FlashPlan plan,
                                                              ScriptedCanFlashTransport& transport,
                                                              fastecu::IClock& clock, RecordingEventSink& events)
{
    ManualCancellationToken cancellation;
    return executor.Execute(plan, transport, clock, cancellation, events);
}

TEST(SubaruTcuHitachiM32rCanExecutor, TransportSetupMatchesLegacyCanConfiguration)
{
    SubaruTcuHitachiM32rCanExecutor executor;
    const auto config = executor.TransportSetup(ReadPlan());

    ASSERT_THAT(config, fastecu::testing::IsOk());
    EXPECT_EQ(config->request_id, 0x7E1U);
    EXPECT_EQ(config->response_id, 0x7E9U);
    EXPECT_EQ(config->bitrate, 500000);
    EXPECT_FALSE(config->extended_id);
}

TEST(SubaruTcuHitachiM32rCanExecutor, KernelAlreadyRunningShortCircuitsAfterFirstFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(Framed(0x7E1, {0x31, 0x02, 0x02, 0x01}), Response(kAlive));
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.ReadTimeouts(), std::vector(2, 2000ms));
    EXPECT_EQ(clock.Elapsed(), 0ms);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FullConnectPreservesFrameOrderPacingAndTimeouts)
{
    std::vector<Call> trace;
    TracingTransport transport{trace};
    ScriptFullConnect(transport);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    MockClock clock;
    TraceSleeps(clock, trace);
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_EQ(transport.ReadTimeouts(), std::vector(kConnectFrames + 1, 2000ms));
    EXPECT_EQ(clock.Elapsed(), 450ms);
    std::vector<Call> expected = ConnectTrace();
    expected.push_back(kWriteCall);
    expected.push_back(kReadCall);
    ExpectTraceEquals(trace, expected);
}

TEST(SubaruTcuHitachiM32rCanExecutor, SuccessfulIdentityResponsesLogDecodedFields)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kInfo, "TCU ID: 1122334455")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kInfo, "CAL ID: CAL")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(fastecu::LogLevel::kInfo, HasSubstr("TCU ID response:")))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(fastecu::LogLevel::kInfo, HasSubstr("CAL ID response:")))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, ShortIdentityFieldsLogAndContinueSafely)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    transport.Exchange(Framed(0x7E0, {0xAA}), Response({0xEA, 0x00, 0x00}));
    transport.Exchange(Framed(0x7E0, {0x09, 0x04}), Response({0x49, 0x04, 0x00}));
    ScriptSession(transport);
    ScriptSeed(transport);
    ScriptKey(transport);
    ScriptJump(transport);
    ScriptRecheck(transport);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kError, "TCU ID response is too short")));
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kError, "CAL ID response is too short")));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalTcuIdMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport, false, true, true);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Wrong response from TCU for TCU ID"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalCalIdMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport, true, false, true);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Wrong response from TCU for CAL ID"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalKernelJumpMismatchStillReachesFinalRecheck)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport, true, true, false);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Wrong response from TCU for kernel jump"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalSessionMismatchStopsAtStepFour)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    ScriptIdentity(transport);
    ScriptSession(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 4U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalSeedMismatchStopsAtStepFive)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    ScriptIdentity(transport);
    ScriptSession(transport);
    ScriptSeed(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 5U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalKeyMismatchStopsAtStepSix)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    ScriptIdentity(transport);
    ScriptSession(transport);
    ScriptSeed(transport);
    ScriptKey(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 6U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, FatalFinalRecheckMismatchStopsAtStepEight)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    ScriptIdentity(transport);
    ScriptSession(transport);
    ScriptSeed(transport);
    ScriptKey(transport);
    ScriptJump(transport);
    ScriptRecheck(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 8U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, SeedResponseShorterThanTenBytesIsBadResponse)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    ScriptIdentity(transport);
    ScriptSession(transport);
    transport.Exchange(Framed(0x7E0, {0x27, 0x01}), Response({0x67, 0x01, 0xDE}));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 5U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, KernelAliveRecheckIsAnEightByteFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptSilentReadWindow(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
}

// --- read path -------------------------------------------------------------

TEST(SubaruTcuHitachiM32rCanExecutor, ReadWindowSetupTargetsTheClampedRegion)
{
    // Divergence 2: the request bytes are 34 04 33 00 80 00 07 80 00 -- the
    // scripted transport rejects any other window, including the legacy
    // 0xFFF00000/0x80000 pair that its own underflow produced.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_EQ(ReadWindowRequest(),
              (Bytes{0x00, 0x00, 0x07, 0xE1, 0x34, 0x04, 0x33, 0x00, 0x80, 0x00, 0x07, 0x80, 0x00}));
}

// T3c: the negative case above is a 3-byte NRC that trips expect_prefix's
// length guard rather than its content compare -- a mutation shortening the
// expected prefix from {0x74,0x20,0x01,0x04} down to {0x74} would survive
// that test entirely, and a TCU answering 74 20 01 03 would be silently
// accepted, dumping against an unconfirmed read window. This response is the
// full 8-byte frame, differing from the valid one at only the last byte, so
// only the content compare can catch it.
TEST(SubaruTcuHitachiM32rCanExecutor, ReadWindowContentMismatchIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindowWrongContent(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, SuccessfulReadRebuildsTheFullRomImage)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    ScriptDumpLoop(transport);
    ScriptStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    // 8 connect frames + window setup + 1920 pages + stop.
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + kPageCount + 1);
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kRomSize);
    // I2: legacy composes ecuCalDef->RomId as "<CALID>_<TCUID>_"
    // (operation.cpp:170, 207); scriptIdentity's fixture bytes decode to TCU
    // ID "1122334455" and CAL ID "CAL" (SuccessfulIdentityResponsesLogDecodedFields
    // pins the same log lines).
    EXPECT_EQ(result->rom_id, std::optional<std::string>("CAL_1122334455_"));

    const Bytes expected = ExpectedImage();
    // Divergence 4: the first 0x8000 bytes are a sized zero buffer.
    EXPECT_EQ(std::ranges::count(bytes::ByteView{*result->read_bytes}.subspan(0, kRegionStart), Byte{0x00}),
              static_cast<std::ptrdiff_t>(kRegionStart));
    // The first and last dumped pages, reconstructed: a shifted header strip
    // moves these even where the total length happens to survive.
    EXPECT_EQ(Slice(*result->read_bytes, kRegionStart, 8), Slice(expected, kRegionStart, 8));
    EXPECT_EQ(Slice(*result->read_bytes, kRomSize - 8, 8), Slice(expected, kRomSize - 8, 8));
    ExpectBytesEqual(*result->read_bytes, expected);
}

// I2: the connect path already tolerates a mismatched TCU ID non-fatally
// (NonFatalTcuIdMismatchStillReachesFinalRecheck); this pins the resulting
// rom_id rather than only the connect's continuation. Chosen behaviour:
// rom_id is reported only when BOTH the TCU ID and the CAL ID decode
// cleanly, never a half-built "<TCUID>_"-only or "<CALID>_"-only id -- a
// caller either gets the well-formed identifier legacy's success path
// produces, or none at all.
TEST(SubaruTcuHitachiM32rCanExecutor, ReadRomIdIsAbsentWhenTheTcuIdDidNotMatch)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport, /*valid_tcu=*/false, /*valid_cal=*/true);
    ScriptReadWindow(transport);
    ScriptDumpLoop(transport);
    ScriptStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, std::nullopt);
}

// Same shape, the short-frame guard instead of a mismatch: both identity
// fields come back too short to decode (mirrors
// ShortIdentityFieldsLogAndContinueSafely), so rom_id must be absent too.
TEST(SubaruTcuHitachiM32rCanExecutor, ReadRomIdIsAbsentWhenIdentityFieldsAreTooShort)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptKernelProbeMiss(transport);
    transport.Exchange(Framed(0x7E0, {0xAA}), Response({0xEA, 0x00, 0x00}));
    transport.Exchange(Framed(0x7E0, {0x09, 0x04}), Response({0x49, 0x04, 0x00}));
    ScriptSession(transport);
    ScriptSeed(transport);
    ScriptKey(transport);
    ScriptJump(transport);
    ScriptRecheck(transport);
    ScriptReadWindow(transport);
    ScriptDumpLoop(transport);
    ScriptStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, std::nullopt);
}

TEST(SubaruTcuHitachiM32rCanExecutor, ReadLoopPacesEveryPageAndKeepsLegacyTimeouts)
{
    std::vector<Call> trace;
    TracingTransport transport{trace};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    ScriptDumpLoop(transport);
    ScriptStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    MockClock clock;
    TraceSleeps(clock, trace);
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(transport.ReadTimeouts(), std::vector(kConnectFrames + 1 + kPageCount + 1, 2000ms));
    // 450 ms of connect pacing plus 1 ms after each of the 1920 pages.
    EXPECT_EQ(clock.Elapsed(), 450ms + (kPageCount * 1ms));
    ExpectTraceEquals(trace, FullReadTrace());
}

TEST(SubaruTcuHitachiM32rCanExecutor, PageResponseWithoutTheDumpServiceIdIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    {
        const auto section = transport.Section("dump loop");
        transport.Exchange(PageRequest(0), PageResponse(0));
        transport.Exchange(PageRequest(1), Response({0x7F, 0xB7, 0x22}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    // The message discriminates the 0xF7 check from the length guard below it:
    // without it, dropping the 0xF7 check still reaches a BadResponse by way
    // of the length guard and this test would pin nothing.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Not(HasSubstr("expected 261"))));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 2);
}

TEST(SubaruTcuHitachiM32rCanExecutor, PageResponseOfTheWrongLengthIsFatal)
{
    // Guard beyond legacy: legacy appended whatever followed the five header
    // bytes, so a short page silently shortened the image it handed back.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    {
        const auto section = transport.Section("dump loop");
        Bytes truncated = PageResponse(0);
        truncated.pop_back();
        transport.Exchange(PageRequest(0), truncated);
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("expected 261")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, StopFrameMismatchIsNotFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    ScriptDumpLoop(transport);
    ScriptStop(transport, false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    ExpectBytesEqual(*result->read_bytes, ExpectedImage());
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Wrong response from TCU for dump stop"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, StopFrameSilenceIsNotFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    ScriptDumpLoop(transport);
    ScriptSilentStop(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("No valid response from TCU for dump stop"))));
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

Byte RomByte(std::size_t index)
{
    return static_cast<Byte>(((index * 7U) + ((index >> 8U) * 13U) + 3U) & 0xFFU);
}

const Bytes& PlaintextRom()
{
    static const Bytes rom = []
    {
        Bytes image(kRomSize);
        for (std::size_t index = 0; index < kRomSize; ++index)
        {
            image[index] = RomByte(index);
        }
        return image;
    }();
    return rom;
}

// The encrypted image every 0xB6 frame must carry, built here from the tables
// spelled out rather than borrowed from production, so a swapped or missing
// table changes this expectation.
const Bytes& EncryptedRom()
{
    static const Bytes rom = []
    {
        // Legacy encrypt_payload (operation.cpp:1002).
        static constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};
        static constexpr std::array<std::uint8_t, 32> kIndexTransformation{
            0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
            0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
        return ssm_protocol::CalculatePayload(PlaintextRom(), static_cast<std::uint32_t>(kRomSize), kEncryptTable,
                                              kIndexTransformation);
    }();
    return rom;
}

FlashPlan WritePlan()
{
    auto plan = BuildSubaruTcuHitachiM32rCanPlan(FlashOperation::kWrite, kProtocol, kMcu, PlaintextRom());
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

Bytes EraseRequest()
{
    return Framed(0x7E1, {0x31, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
}

Bytes BlockWindowRequest(const BlockSpec& block)
{
    Bytes payload{0x34, 0x04, 0x33};
    bytes::AppendU24Be(payload, block.start);
    bytes::AppendU24Be(payload, block.len);
    return FramedBytes(0x7E1, payload);
}

Bytes DataRequest(std::uint32_t address)
{
    Bytes payload{0xB6};
    bytes::AppendU24Be(payload, address);
    const Bytes& rom = EncryptedRom();
    const auto offset = static_cast<std::ptrdiff_t>(address);
    payload.insert(payload.end(), rom.begin() + offset, rom.begin() + offset + kWriteFrameSize);
    return FramedBytes(0x7E1, payload);
}

Bytes CloseRequest()
{
    return Framed(0x7E1, {0x37});
}

Bytes ChecksumRequest()
{
    return Framed(0x7E1, {0x31, 0x02, 0x02, 0x01});
}

// Positive answers carry only the byte(s) legacy actually inspects, so a
// production check widened beyond legacy's would fail here; every negative
// answer below is the same length as its positive twin and differs in exactly
// the byte under test.
void ScriptErase(ScriptedCanFlashTransport& transport, std::optional<Bytes> answer = Response({0x31, 0x02, 0x01}))
{
    const auto section = transport.Section("erase");
    if (answer.has_value())
    {
        transport.Exchange(EraseRequest(), *answer);
        return;
    }
    transport.ExpectWrite(EraseRequest());
    transport.QueueNoFrame();
}

void ScriptBlockWindow(ScriptedCanFlashTransport& transport, const BlockSpec& block, bool valid = true)
{
    const auto section = transport.Section("block window setup");
    transport.Exchange(BlockWindowRequest(block), valid ? Response({0x74}) : Response({0x75}));
}

void ScriptBlockData(ScriptedCanFlashTransport& transport, const BlockSpec& block)
{
    const auto section = transport.Section("block data frames");
    for (std::uint32_t offset = 0; offset < block.len; offset += kWriteFrameSize)
    {
        transport.Exchange(DataRequest(block.start + offset), Response({0xF6}));
    }
}

void ScriptClose(ScriptedCanFlashTransport& transport, bool valid = true)
{
    const auto section = transport.Section("block close");
    transport.Exchange(CloseRequest(), valid ? Response({0x77}) : Response({0x78}));
}

void ScriptChecksum(ScriptedCanFlashTransport& transport, std::optional<Bytes> answer = Response({0x71, 0x02, 0x02}))
{
    const auto section = transport.Section("block checksum");
    if (answer.has_value())
    {
        transport.Exchange(ChecksumRequest(), *answer);
        return;
    }
    transport.ExpectWrite(ChecksumRequest());
    transport.QueueNoFrame();
}

void ScriptBlock(ScriptedCanFlashTransport& transport, const BlockSpec& block)
{
    ScriptBlockWindow(transport, block);
    ScriptBlockData(transport, block);
    ScriptClose(transport);
    ScriptChecksum(transport);
}

void ScriptFullWrite(ScriptedCanFlashTransport& transport)
{
    ScriptFullConnect(transport);
    ScriptErase(transport);
    for (const BlockSpec& block : kFlashedBlocks)
    {
        ScriptBlock(transport, block);
    }
}

// 8 connect frames + erase + per block (window + N data + close + checksum).
constexpr std::size_t kFullWriteWrites =
    kConnectFrames + 1 + (kFlashedBlocks.size() * 3) + static_cast<std::size_t>(kTotalDataFrames);

class RecordingTransport final : public ScriptedCanFlashTransport
{
  public:
    RecordingTransport() : ScriptedCanFlashTransport(ScriptedTransportInitialState::kOpen)
    {
    }

    Status Write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        return ScriptedCanFlashTransport::Write(data, cancellation);
    }

    std::vector<Bytes> writes;
};

std::uint32_t AddressAt(const Bytes& frame, std::size_t offset)
{
    return (static_cast<std::uint32_t>(frame[offset]) << 16U) | (static_cast<std::uint32_t>(frame[offset + 1]) << 8U) |
           static_cast<std::uint32_t>(frame[offset + 2]);
}

// Every address a 0x34 04 33 window-setup frame asked the kernel to open.
std::vector<std::uint32_t> WindowStarts(const std::vector<Bytes>& writes)
{
    std::vector<std::uint32_t> starts;
    for (const Bytes& frame : writes)
    {
        if (frame.size() >= 13 && frame[4] == 0x34 && frame[5] == 0x04 && frame[6] == 0x33)
        {
            starts.push_back(AddressAt(frame, 7));
        }
    }
    return starts;
}

// Every address a 0xB6 data frame wrote 128 bytes to.
std::vector<std::uint32_t> DataAddresses(const std::vector<Bytes>& writes)
{
    std::vector<std::uint32_t> addresses;
    for (const Bytes& frame : writes)
    {
        if (frame.size() == 8 + kWriteFrameSize && frame[4] == 0xB6)
        {
            addresses.push_back(AddressAt(frame, 5));
        }
    }
    return addresses;
}

constexpr Call kEraseSleepCall{CallKind::kSleep, 500ms};
constexpr Call kEraseReadCall{CallKind::kRead, 200ms};
constexpr Call kDataSleepCall{CallKind::kSleep, 200ms};
constexpr Call kDataReadCall{CallKind::kRead, 500ms};
constexpr Call kLongReadCall{CallKind::kRead, 800ms};
constexpr Call kCloseSleepCall{CallKind::kSleep, 100ms};

std::vector<Call> FullWriteTrace()
{
    std::vector<Call> trace = ConnectTrace();
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

std::vector<std::chrono::milliseconds> FullWriteReadTimeouts()
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
    bytes::AppendU24Be(plaintext_frame, kRegionStart);
    plaintext_frame.insert(plaintext_frame.end(), PlaintextRom().begin() + kRegionStart,
                           PlaintextRom().begin() + kRegionStart + kWriteFrameSize);
    ASSERT_NE(DataRequest(kRegionStart), FramedBytes(0x7E1, plaintext_frame));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.Section("block data frames");
        transport.Exchange(DataRequest(kRegionStart), Response({0xF6}));
        transport.Exchange(DataRequest(kRegionStart + kWriteFrameSize), Response({0xF5}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + 2);
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseRequestMatchesLegacyBytes)
{
    EXPECT_EQ(EraseRequest(), (Bytes{0x00, 0x00, 0x07, 0xE1, 0x31, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0], false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    // The run got past the erase and stopped at the first block window, so the
    // erase bytes and its positive answer are both pinned.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1);
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseResponseShorterThanSevenBytesIsFatalBeforeAnyReflashFrame)
{
    // Deliberate divergence 5: legacy erase_mem indexed received.at(4..6) with
    // no length guard and had its `return STATUS_ERROR` commented out, so a
    // failed erase was logged and reflash proceeded onto unerased flash.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport, Response({0x31, 0x02}));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("response is too short")));
    EXPECT_TRUE(transport.ScriptConsumed());
    // Connect plus the erase itself and nothing more: no window, no 0xB6.
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Do not panic"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseContentMismatchIsFatalBeforeAnyReflashFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    // Same length as the positive answer, differing in exactly the last byte.
    ScriptErase(transport, Response({0x31, 0x02, 0x02}));
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Not(HasSubstr("too short"))));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Do not panic"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, EraseSilenceIsFatalBeforeAnyReflashFrame)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport, std::nullopt);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Do not panic"))));
}

// T4a: a port/transport error is a comms fault, not TCU-state ambiguity.
TEST(SubaruTcuHitachiM32rCanExecutor, EraseTransportErrorDoesNotLogTheDoNotPanicWarning)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    {
        const auto section = transport.Section("erase (transport error)");
        transport.ExpectWrite(EraseRequest());
        transport.QueueError(ErrorKind::kDisconnected, "adapter gone mid-erase");
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kDisconnected, HasSubstr("adapter gone mid-erase")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Do not panic")))));
}

// Cancels a shared ManualCancellationToken the instant a chosen write
// completes -- used to reach exchange_optional's own "cancelled after write"
// checkpoint precisely, before any read is ever attempted. (Tripping instead
// on the pre-read sleep, as trace_sleeps_then_trip does elsewhere in this
// file, does not isolate this checkpoint here: the scripted transport's own
// read() observes the now-cancelled token and fails first with its own
// "scripted CAN read cancelled" message, never reaching exchange_optional's
// "cancelled after read" check.)
class TripAfterWriteCountTransport final : public ScriptedCanFlashTransport
{
  public:
    TripAfterWriteCountTransport(std::size_t trip_after_writes, ManualCancellationToken& cancellation)
        : ScriptedCanFlashTransport(ScriptedTransportInitialState::kOpen), trip_after_writes_(trip_after_writes),
          cancellation_(cancellation)
    {
    }

    Status Write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        const Status result = ScriptedCanFlashTransport::Write(data, cancellation);
        if (++writes_ == trip_after_writes_)
        {
            cancellation_.Cancel();
        }
        return result;
    }

  private:
    std::size_t trip_after_writes_;
    std::size_t writes_ = 0;
    ManualCancellationToken& cancellation_;
};

// T4a: cancellation is not a content-mismatch. Trips right after the erase's
// own write (the 9th write overall) completes, so exchange_optional's
// generic "cancelled after write" checkpoint fires before any read is ever
// attempted -- not expect_prefix's content compare -- exactly the case
// legacy has no equivalent of.
TEST(SubaruTcuHitachiM32rCanExecutor, EraseCancellationDoesNotLogTheDoNotPanicWarning)
{
    ManualCancellationToken cancellation;
    TripAfterWriteCountTransport transport{kConnectFrames + 1, cancellation};
    ScriptFullConnect(transport);
    transport.ExpectWrite(EraseRequest());
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.Execute(WritePlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, HasSubstr("cancelled after write")));
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1);
    EXPECT_THAT(events.logs, Not(Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Do not panic")))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, WriteFlashesBlocksThreeThroughTenAndNothingElse)
{
    RecordingTransport transport;
    ScriptFullWrite(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kFullWriteWrites);
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    // I2: legacy only assigns ecuCalDef->RomId when cmd_type == "read"
    // (operation.cpp:168, 205); connect_bootloader() decodes the TCU/CAL ID
    // identically on every connect, but the write result must not surface it.
    EXPECT_EQ(result->rom_id, std::nullopt);
    EXPECT_THAT(events.logs, Contains(Pair(fastecu::LogLevel::kInfo, "TCU ID: 1122334455")));

    const auto *device = fastecu::flash::FindFlashDevice(kMcu);
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->numblocks, 11U);

    const std::vector<std::uint32_t> opened = WindowStarts(transport.writes);
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

    const std::vector<std::uint32_t> written = DataAddresses(transport.writes);
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
    ScriptFullWrite(transport);
    SubaruTcuHitachiM32rCanExecutor executor;
    MockClock clock;
    TraceSleeps(clock, trace);
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(transport.ReadTimeouts(), FullWriteReadTimeouts());
    // 450 ms of connect pacing, 500 ms before the erase read, 200 ms before
    // each of the 3840 data reads, and 100 ms between each close and checksum.
    EXPECT_EQ(clock.Elapsed(), 450ms + 500ms + (kTotalDataFrames * 200ms) + (kFlashedBlocks.size() * 100ms));
    ExpectTraceEquals(trace, FullWriteTrace());
}

TEST(SubaruTcuHitachiM32rCanExecutor, BlockWindowMismatchIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0], false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1);
    EXPECT_EQ(BlockWindowRequest(kFlashedBlocks[0]),
              (Bytes{0x00, 0x00, 0x07, 0xE1, 0x34, 0x04, 0x33, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00}));
}

TEST(SubaruTcuHitachiM32rCanExecutor, NonFatalCloseAnswerIsRetriedAndTheBlockStillCompletes)
{
    // Legacy treats a non-0x77 close reply as non-fatal (its return is
    // commented out, operation.cpp:861) and simply tries again.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    ScriptBlockData(transport, kFlashedBlocks[0]);
    ScriptClose(transport, false);
    ScriptClose(transport, true);
    ScriptChecksum(transport);
    // Block 4 stops the run so the assertions stay on block 3's close.
    ScriptBlockWindow(transport, kFlashedBlocks[1], false);
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    const std::size_t block3_frames = kFlashedBlocks[0].len / kWriteFrameSize;
    // connect + erase + window + data + two closes + checksum + block 4 window.
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + block3_frames + 2 + 1 + 1);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Wrong response from TCU for block close"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, CloseRetriesExhaustAfterTwentyAttemptsAndNoChecksumFollows)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    ScriptBlockData(transport, kFlashedBlocks[0]);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        ScriptClose(transport, false);
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("did not close after 20")));
    EXPECT_TRUE(transport.ScriptConsumed());
    const std::size_t block3_frames = kFlashedBlocks[0].len / kWriteFrameSize;
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + block3_frames + kRetryAttempts);
    // Exhaustion stops before the checksum, so the 100 ms close-to-checksum
    // pause never runs either.
    EXPECT_EQ(clock.Elapsed(), 450ms + 500ms + (block3_frames * 200ms));
}

TEST(SubaruTcuHitachiM32rCanExecutor, ChecksumAnswerDifferingInOneByteFailsTheBlock)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    ScriptBlockData(transport, kFlashedBlocks[0]);
    ScriptClose(transport);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        ScriptChecksum(transport, Response({0x71, 0x02, 0x03}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("checksum failed after 20 attempts")));
    EXPECT_TRUE(transport.ScriptConsumed());
    const std::size_t block3_frames = kFlashedBlocks[0].len / kWriteFrameSize;
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + block3_frames + 1 + kRetryAttempts);
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("Wrong response from TCU for block checksum"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, ChecksumAnswerShorterThanSevenBytesFailsTheBlock)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    ScriptBlockData(transport, kFlashedBlocks[0]);
    ScriptClose(transport);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        ScriptChecksum(transport, Response({0x71, 0x02}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("checksum failed after 20 attempts")));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuHitachiM32rCanExecutor, MissingChecksumAnswerFailsTheBlock)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    ScriptBlockData(transport, kFlashedBlocks[0]);
    ScriptClose(transport);
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        ScriptChecksum(transport, std::nullopt);
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("checksum failed after 20 attempts")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(fastecu::LogLevel::kError, HasSubstr("No valid response from TCU for block checksum"))));
}

TEST(SubaruTcuHitachiM32rCanExecutor, DataFrameMismatchIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.Section("block data frames");
        transport.Exchange(DataRequest(kRegionStart), Response({0xF6}));
        // Same length as the positive answer, differing in the service id.
        transport.Exchange(DataRequest(kRegionStart + kWriteFrameSize), Response({0xF5}));
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("wrong response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + 2);
}

TEST(SubaruTcuHitachiM32rCanExecutor, DataFrameSilenceIsFatal)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.Section("block data frames");
        transport.ExpectWrite(DataRequest(kRegionStart));
        transport.QueueNoFrame();
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("no response from TCU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + 1);
}

// A transport with no script at all: it answers every request positively, so
// nothing stops a widened block range except the assertions in the test below.
// Without it, "blocks 0-2 are not flashed" would rest entirely on the scripted
// transport rejecting the extra frames, and the explicit Not(Contains(...))
// assertions would never run because the IsOk assertion aborts first.
class PermissiveTransport final : public ScriptedCanFlashTransport
{
  public:
    PermissiveTransport() : ScriptedCanFlashTransport(ScriptedTransportInitialState::kOpen)
    {
    }

    Status Write(bytes::ByteView data, const fastecu::ICancellationToken& /*cancellation*/) override
    {
        writes.emplace_back(data.begin(), data.end());
        return {};
    }

    fastecu::Result<std::optional<Bytes>> Read(std::chrono::milliseconds /*timeout*/,
                                               const fastecu::ICancellationToken& /*cancellation*/) override
    {
        return std::optional<Bytes>{AnswerFor(writes.empty() ? Bytes{} : writes.back())};
    }

    std::vector<Bytes> writes;

  private:
    static Bytes AnswerFor(const Bytes& request)
    {
        if (request.size() < 5)
        {
            return Response({0x7F});
        }
        switch (request[4])
        {
        case 0x31:
            // The erase (31 02 01 ...) echoes; every other 0x31 is a kernel
            // liveness or block checksum probe.
            if (request.size() > 6 && request[6] == 0x01)
            {
                return Response({0x31, 0x02, 0x01});
            }
            return Response({0x71, 0x02, 0x02, 0x03});
        case 0x34:
            return Response({0x74});
        case 0xB6:
            return Response({0xF6});
        case 0x37:
            return Response({0x77});
        default:
            return Response({0x7F});
        }
    }
};

TEST(SubaruTcuHitachiM32rCanExecutor, WithNoScriptToStopItTheWriteStillNeverTouchesABootloaderBlock)
{
    PermissiveTransport transport;
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, WritePlan(), transport, clock, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());

    const auto *device = fastecu::flash::FindFlashDevice(kMcu);
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->numblocks, 11U);

    const std::vector<std::uint32_t> opened = WindowStarts(transport.writes);
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

    const std::vector<std::uint32_t> written = DataAddresses(transport.writes);
    EXPECT_EQ(written.size(), kTotalDataFrames);
    EXPECT_EQ(std::ranges::count_if(written, [](std::uint32_t address) { return address < kRegionStart; }), 0)
        << "a 0xB6 data frame targeted the bootloader region below 0x8000";
    EXPECT_EQ(
        std::ranges::count_if(written, [](std::uint32_t address) { return address + kWriteFrameSize > kRomSize; }), 0)
        << "a 0xB6 data frame ran past the end of the 0x80000 ROM";
}

// --- cancellation and failure paths -----------------------------------------
//
// Every cancellation test below returns ErrorKind::kCancelled, which by itself
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
// eight-exchange connect sequence), not copied from any other family: each
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
void TraceSleepsThenTrip(MockClock& clock, std::vector<Call>& trace, std::size_t trip_at,
                         ManualCancellationToken& cancellation)
{
    ON_CALL(clock, Sleep)
        .WillByDefault(
            [&clock, &trace, trip_at, &cancellation](std::chrono::milliseconds duration,
                                                     const fastecu::ICancellationToken& token)
            {
                const Status result = clock.Fake().Sleep(duration, token);
                trace.push_back({CallKind::kSleep, duration});
                if (trace.size() == trip_at)
                {
                    cancellation.Cancel();
                }
                return result;
            });
}

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

    void Progress(int done, int total) override
    {
        RecordingEventSink::Progress(done, total);
        if (++calls_ == trip_after_calls_)
        {
            cancellation_.Cancel();
        }
    }

  private:
    std::size_t trip_after_calls_;
    std::size_t calls_ = 0;
    ManualCancellationToken& cancellation_;
};

// connectTrace() + the window setup + `pages` full pages (write, read, the
// 1 ms per-page pause) -- the same shape fullReadTrace() builds, truncated.
std::vector<Call> ReadTracePrefix(std::size_t pages)
{
    std::vector<Call> trace = ConnectTrace();
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
std::vector<Call> WriteTracePrefixThroughFrames(std::size_t frames)
{
    std::vector<Call> trace = ConnectTrace();
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
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    {
        const auto section = transport.Section("dump loop");
        for (std::uint32_t page = 0; page < kCancelAfterPages; ++page)
        {
            transport.Exchange(PageRequest(page), PageResponse(page));
        }
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    ManualCancellationToken cancellation;
    const std::vector<Call> expected_prefix = ReadTracePrefix(kCancelAfterPages);
    MockClock clock;
    TraceSleepsThenTrip(clock, trace, expected_prefix.size(), cancellation);
    RecordingEventSink events;

    const auto result = executor.Execute(ReadPlan(), transport, clock, cancellation, events);

    // "cancelled during ROM read" is read_rom's top-of-loop checkpoint --
    // distinct from the generic "cancelled before/after write/read" messages
    // exchange_optional's own checks would produce if this checkpoint were
    // missing and one of those caught it instead.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, HasSubstr("cancelled during ROM read")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + kCancelAfterPages);
    ExpectTraceEquals(trace, expected_prefix);
}

TEST(SubaruTcuHitachiM32rCanExecutor, CancellationDuringTheWriteLoopStopsAtTheNextFrameBoundary)
{
    constexpr auto kCancelAfterFrames = static_cast<std::uint32_t>(kConnectFrames);
    std::vector<Call> trace;
    TracingTransport transport{trace};
    ScriptFullConnect(transport);
    ScriptErase(transport);
    ScriptBlockWindow(transport, kFlashedBlocks[0]);
    {
        const auto section = transport.Section("block data frames");
        for (std::uint32_t frame = 0; frame < kCancelAfterFrames; ++frame)
        {
            transport.Exchange(DataRequest(kFlashedBlocks[0].start + (frame * kWriteFrameSize)), Response({0xF6}));
        }
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    ManualCancellationToken cancellation;
    MockClock clock;
    TraceSleeps(clock, trace);
    TripAfterProgressCallsEventSink events{kCancelAfterFrames, cancellation};

    const auto result = executor.Execute(WritePlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, HasSubstr("cancelled during block write")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 1 + kCancelAfterFrames);
    ExpectTraceEquals(trace, WriteTracePrefixThroughFrames(kCancelAfterFrames));
}

TEST(SubaruTcuHitachiM32rCanExecutor, PlanForAnotherFamilyIsRejectedBeforeTransportSetupOrTheWire)
{
    // The K-Line sibling: identical MCU and ROM geometry, differing only in
    // the family tag and the transport kind -- exactly the shape of plan a
    // missing or misplaced check_family() call would let slip through.
    auto plan =
        BuildSubaruTcuHitachiM32rKlinePlan(FlashOperation::kRead, "sub_tcu_hitachi_m32r_kline", kMcu, std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ASSERT_EQ(plan->Family(), FlashFamily::kSubaruTcuHitachiM32rKline);

    SubaruTcuHitachiM32rCanExecutor executor;
    const auto setup_result = executor.TransportSetup(*plan);
    EXPECT_THAT(setup_result,
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("plan family does not match")));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ManualCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto execute_result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(execute_result,
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("plan family does not match")));
    // An empty script: any write at all would fail with a "ran past the end
    // of the script" Internal error instead, so writesConsumed() == 0 and a
    // consumed (trivially, empty) script together prove the wire was never
    // touched.
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

// M2: exercises the real, reachable TestWrite-rejection layer at the
// executor boundary -- the second of this family's two independent
// TestWrite checks (subaru_tcu_hitachi_m32r_can_plan.cpp's validator), which
// the executor's own execute() consumes on every plan. Bypasses the plan
// BUILDER's own TestWrite check (which would otherwise refuse to construct
// this plan at all -- see SubaruTcuHitachiM32rCanPlan.RejectsTestWriteAsUnsupported)
// by constructing FlashPlanFields directly through validate_and_build,
// mirroring SubaruTcuHitachiM32rCanPlan.ValidatorRejectsTestWriteDirectlyConstructed
// but through the executor rather than the standalone validator, so the
// "executor boundary" consumer this family's rejection count depends on is
// pinned rather than only asserted in a comment. This directly-constructed
// plan is also what proves execute_transfer's own new TestWrite guard is
// unreachable from any public entry: execute() rejects it here, before
// execute_transfer -- the guard inside execute_transfer -- ever runs.
TEST(SubaruTcuHitachiM32rCanExecutor, ExecuteRejectsTestWriteEvenWhenDirectlyConstructed)
{
    using fastecu::flash::FlashPlanFields;
    using fastecu::flash::MemoryRegion;
    using fastecu::flash::SubaruTcuHitachiM32rCanPlan;
    using fastecu::flash::TransportKind;
    using fastecu::flash::ValidateAndBuild;

    constexpr MemoryRegion kWindow{.start = kRegionStart, .length = kRegionLength};
    FlashPlanFields fields{
        .operation = FlashOperation::kTestWrite,
        .family = FlashFamily::kSubaruTcuHitachiM32rCan,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcu),
        .transfer_region = kWindow,
        .erase_regions = {kWindow},
        .image = bytes::Bytes(kRomSize, Byte{0x00}),
        .kernel = std::nullopt,
        .family_plan = SubaruTcuHitachiM32rCanPlan{.request_id = 0x7E1,
                                                   .response_id = 0x7E9,
                                                   .bitrate = 500000,
                                                   .extended_id = false,
                                                   .page_size = kPageSize,
                                                   .write_frame_size = 128U},
        .confirmations = {},
    };
    auto built = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(built, fastecu::testing::IsOk());

    SubaruTcuHitachiM32rCanExecutor executor;
    const auto setup_result = executor.TransportSetup(*built);
    EXPECT_THAT(setup_result, fastecu::testing::IsErr(ErrorKind::kUnsupported));

    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ManualCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto execute_result = executor.Execute(*built, transport, clock, cancellation, events);

    EXPECT_THAT(execute_result, fastecu::testing::IsErr(ErrorKind::kUnsupported));
    // No transport activity at all: the validator rejects before connect_bootloader.
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruTcuHitachiM32rCanExecutor, TransportErrorDuringTheReadLoopStopsWithoutFurtherPageRequests)
{
    // A port-level error (adapter disconnect, USB drop, ...) is not an ECU
    // content mismatch: it must propagate immediately with its own kind and
    // message, distinct from PageResponseWithoutTheDumpServiceIdIsFatal's
    // BadResponse and from every cancellation checkpoint's Cancelled.
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptFullConnect(transport);
    ScriptReadWindow(transport);
    {
        const auto section = transport.Section("dump loop");
        transport.Exchange(PageRequest(0), PageResponse(0));
        transport.ExpectWrite(PageRequest(1));
        transport.QueueError(ErrorKind::kDisconnected, "adapter gone mid-dump");
    }
    SubaruTcuHitachiM32rCanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = Execute(executor, ReadPlan(), transport, clock, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kDisconnected, HasSubstr("adapter gone mid-dump")));
    EXPECT_TRUE(transport.ScriptConsumed());
    // Connect + window + page 0 + page 1's write (whose read is what fails):
    // the error surfaces on the very next exchange and no further page is
    // ever requested.
    EXPECT_EQ(transport.WritesConsumed(), kConnectFrames + 1 + 2);
}
} // namespace
