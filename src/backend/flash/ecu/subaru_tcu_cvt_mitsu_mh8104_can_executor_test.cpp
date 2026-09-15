// Equivalence tests for SubaruTcuCvtMitsuMh8104CanExecutor, the portable
// replacement for flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp.
//
// This family's defining quirk (unlike its MH8111 sibling): every response
// check after the kernel-alive probe is followed by a commented-out
// `// return STATUS_ERROR;` in legacy, so ANY ECU response content is
// tolerated -- only a genuine transport-level failure (timeout/disconnect/
// cancellation) between exchanges stops the executor.
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_tcu_cvt_mitsu_mh8104_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruTcuCvtMitsuMh8104CanExecutor;
using fastecu::flash::SubaruTcuCvtMitsuMh8104CanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8104_can";
constexpr std::string_view kMcu = "MH8104";
constexpr std::uint32_t kWindowStart = 0x8000;
constexpr std::uint32_t kWindowLength = 0x78000;
constexpr std::uint32_t kImageSize = 0x80000;

// This family's own request/reply envelope -- every exchange is sent on
// 0x7e1/0x7e9.
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e1);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes request(std::initializer_list<bytes::Byte> payload)
{
    return request(bytes::ByteView(payload.begin(), payload.size()));
}
bytes::Bytes response(bytes::ByteView tail)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e9);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    return response(bytes::ByteView(tail.begin(), tail.size()));
}

fastecu::Result<fastecu::flash::FlashPlan> readPlan()
{
    return build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> writePlan(bytes::Bytes rom)
{
    return build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_tcu_cvt_mitsu_mh8104_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_mitsu_mh8104_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation, std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruTcuCvtMitsuMh8104Can;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kWindowStart, kWindowLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kWindowStart, kWindowLength}};
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = SubaruTcuCvtMitsuMh8104CanPlan{0x7e1, 0x7e9, 500000, false};
    return fastecu::flash::validate_and_build(std::move(fields));
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload) -- not read back from the executor's own translation unit.
// Identical to the sibling MH8111 family's own tables, re-confirmed against
// MH8104's own legacy source directly, not assumed.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
                                                      0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes seedKey(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

// calculatePayload's Feistel structure is memoryless per 4-byte word
// (position-independent), so this single helper both (a) pre-encrypts a
// known plaintext into the wire bytes a scripted read reply must carry for
// the executor's decrypt step to recover it, and (b) computes the wire
// bytes a write must carry for a known plaintext image.
bytes::Bytes toWire(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptTable,
                                         kIndexTransformation);
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// Scripts the alive probe (0x31/0x02/0x02/0x01) with a MISS reply -- present
// but not matching "already running" -- so connect falls through into the full
// init sequence.
void scriptAliveProbeMiss(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("alive probe miss");
    transport.exchange(request({0x31, 0x02, 0x02, 0x01}), response({0x71, 0x00, 0x00, 0x00}));
}

// TCU ID 0xAA / CAL ID 0x09/0x04: both retried up to 6 times, content-blind --
// scripted with a single successful reply each since the executor stops
// retrying on the first non-empty one.
void scriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("identity queries");
    transport.exchange(request({0xAA}), response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session 0x10/0x43, content-blind.
void scriptSession(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("session");
    transport.exchange(request({0x10, 0x43}), response({0x50, 0x43}));
}

// Seed (0x27/0x01) and seed key (0x27/0x02), both content-blind (single-shot,
// fatal only on a genuine transport failure).
void scriptSeedAndKey(ScriptedCanFlashTransport& transport, bytes::ByteView seed, bytes::ByteView key)
{
    const auto section = transport.section("seed and key");
    bytes::Bytes seedResponse{0x67, 0x01};
    seedResponse.insert(seedResponse.end(), seed.begin(), seed.end());
    transport.exchange(request({0x27, 0x01}), response(seedResponse));

    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.exchange(request(keyRequest), response({0x67, 0x02}));
}

// Jump 0x10/0x42, content-blind.
void scriptJump(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("jump");
    transport.exchange(request({0x10, 0x42}), response({0x50, 0x42}));
}

// Alive re-check: sent 0x34/0x04/0x33/0x00/0x00/0x00/0x08/ 0x00/0x00, checked
// with legacy's buggy `&&` condition against 0x74/0x20/0x01/0x04 -- scripted
// here with the "expected" reply (0x74/0x20/0x01/0x04), which per the literal
// `&&` bug does NOT log "Kernel verified to be running" (since none of the
// four bytes differ), but still succeeds unconditionally either way.
void scriptAliveRecheck(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("alive recheck");
    transport.exchange(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       response({0x74, 0x20, 0x01, 0x04}));
}

// Scripts the full connect_bootloader sequence past a missed alive probe.
void scriptFullConnect(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("full connect");
    scriptAliveProbeMiss(transport);
    scriptIdentityQueries(transport);
    scriptSession(transport);
    const bytes::Bytes key = seedKey(kSeed);
    scriptSeedAndKey(transport, kSeed, key);
    scriptJump(transport);
    scriptAliveRecheck(transport);
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem):
// sent 0x35-prefixed, checked against a 0x75-prefixed reply, content-blind.
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("dump setup");
    transport.exchange(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       response({0x75, 0x20, 0x01, 0x01}));
}

// Scripts the chunked 0xB7 dump sweep over [start, start+length) at
// `pagesize`-byte pages, each page filled with `fill` (plaintext -- the
// scripted wire bytes are toWire(fill-page), decrypted back by the
// executor).
void scriptFlashDump(ScriptedCanFlashTransport& transport, std::uint32_t start, std::uint32_t length,
                     std::uint32_t pagesize, bytes::Byte fill)
{
    const auto section = transport.section("flash dump");
    const bytes::Bytes plainPage(pagesize, fill);
    const bytes::Bytes wirePage = toWire(plainPage);
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        bytes::Bytes reply = response({0xF7});
        reply.insert(reply.end(), wirePage.begin(), wirePage.end());
        transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(addr))), reply);
    }
}

// Scripts the "Sending stop command..." exchange (legacy read_mem): content-
// blind, succeeding on the first non-empty reply.
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("stop command");
    transport.exchange(request({0x37}), response({0x77}));
}

// Connect plus read_mem's dump-setup request, registered as an expected write
// only -- its reply is left for the caller to queue, so the same script
// serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there. The dump-setup exchange is content-blind but still fatal on a
// genuine transport failure (single_shot propagates it unconditionally, see
// dump_flash_range), so this is a safe, shallow cut point.
void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& transport)
{
    scriptFullConnect(transport);
    const auto section = transport.section("dump setup (first request only)");
    transport.exchange(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
}

// Scripts erase_mem's single exchange: NOT a retry loop, unlike the sibling
// MH8111 family's own erase.
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("erase memory");
    transport.exchange(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}), response({0x71, 0x01, 0x02}));
}

bool containsLog(const RecordingEventSink& events, std::string_view substring)
{
    return std::any_of(events.logs.begin(), events.logs.end(),
                       [&](const auto& entry) { return entry.second.find(substring) != std::string::npos; });
}

bytes::Bytes writeRom()
{
    bytes::Bytes rom(kImageSize, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>(i);
    }
    return rom;
}

// Records every ctx.clock.sleep() call's ms argument, so the erase step's
// 8000ms/5000ms delays can be asserted without a real multi-second wait.
class RecordingClock final : public FakeClock
{
  public:
    fastecu::Status sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(duration);
        return FakeClock::sleep(duration, cancellation);
    }
    std::vector<std::chrono::milliseconds> sleep_calls;
};

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ConnectSkipsTheRestWhenKernelAlreadyRunning)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.exchange(request({0x31, 0x02, 0x02, 0x01}), response({0x71, 0x02, 0x02, 0x03}));
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, kWindowLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(containsLog(events, "Kernel already running"));
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ConnectSucceedsEvenWhenEveryDiagnosticResponseIsWrong)
{
    // Pins this family's defining quirk: every exchange after the
    // alive-probe miss gets a deliberately wrong/negative-shaped reply
    // (never a transport error), and execute() still proceeds all the way
    // through connect_bootloader into the read phase.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptAliveProbeMiss(transport);

    transport.exchange(request({0xAA}), response({0x7F, 0xAA, 0x11})); // negative response, still "a reply"

    transport.exchange(request({0x09, 0x04}), response({0x7F, 0x09, 0x11}));

    transport.exchange(request({0x10, 0x43}), response({0x7F, 0x10, 0x22})); // wrong content

    // Still shaped like a seed reply so a key can be computed, but wrong
    // SID -- this family does not validate the SID before using the bytes.
    // The executor extracts the 4 seed bytes at stripped-PDU offsets [2..5]
    // (uds::payload(pdu)[1..4], mirroring legacy's received.at(6..9)), i.e.
    // {0x33, 0xAA, 0xBB, 0xCC} here, NOT the trailing 4 bytes of this
    // 7-byte reply -- legacy indexes by absolute position, not by "the
    // seed field of a well-formed 0x67/0x01 reply".
    transport.exchange(request({0x27, 0x01}), response({0x7F, 0x27, 0x33, 0xAA, 0xBB, 0xCC, 0xDD}));

    const bytes::Bytes seed{0x33, 0xAA, 0xBB, 0xCC};
    const bytes::Bytes key = seedKey(seed);
    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.exchange(request(keyRequest), response({0x7F, 0x27, 0x35}));

    transport.exchange(request({0x10, 0x42}), response({0x7F, 0x10, 0x22}));

    transport.exchange(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}), response({0x7F, 0x34, 0x11}));

    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, kWindowLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kWindowStart + kWindowLength);
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ConnectPropagatesATimeoutBetweenExchanges)
{
    // A genuine transport-level timeout (empty scripted frame) at the
    // seed-key exchange DOES stop the executor -- distinguishing "ECU said
    // no" (tolerated) from "nothing came back at all" (still fatal).
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptAliveProbeMiss(transport);
    scriptIdentityQueries(transport);
    scriptSession(transport);

    bytes::Bytes seedResponse{0x67, 0x01};
    seedResponse.insert(seedResponse.end(), kSeed.begin(), kSeed.end());
    transport.exchange(request({0x27, 0x01}), response(seedResponse));

    const bytes::Bytes key = seedKey(kSeed);
    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.exchange(request(keyRequest));
    transport.queue_no_frame();

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ReadReturnsTheWindowPaddedWithFF)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, kWindowLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kWindowStart + kWindowLength);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->begin() + kWindowStart,
                            [](bytes::Byte b) { return b == 0xFF; }));
    EXPECT_TRUE(std::all_of(result->read_bytes->begin() + kWindowStart, result->read_bytes->end(),
                            [](bytes::Byte b) { return b == 0x5A; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    cancellation.cancel();

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_mem's dump-setup exchange -- a single_shot call whose
// generic transport-failure propagation is the same mechanism at either call
// site -- this pins a transport error raised *inside* the 0xB7 dump-chunk
// loop, a different call site inside a `for` loop carrying its own
// cancellation-check and progress-accumulation state, distinguishing a hard
// transport fault from a merely-wrong ECU reply, which this family tolerates
// everywhere else.
TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(kWindowStart))));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, WriteFlashesTheBlockToleratingEveryContentMismatch)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);

    // Erase: scripted with a deliberately WRONG content reply -- still
    // succeeds (non-fatal), and the 8000ms/5000ms delays are asserted via
    // RecordingClock below, not a real wait.
    transport.exchange(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}), response({0x7F, 0x31, 0x22}));

    // reflash_block setup: the retry loop is content-blind -- ANY reply, right
    // or wrong, stops it after the FIRST attempt (it is presence, not content,
    // that ends the retry), so only one attempt is scripted here, with a
    // deliberately wrong reply.
    transport.exchange(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                bytes::u24(kWindowStart), bytes::u24(kWindowLength))),
                       response({0x7F, 0x34, 0x22}));

    const bytes::ByteView blockPlain = bytes::ByteView(rom).subspan(kWindowStart, kWindowLength);
    const bytes::Bytes encrypted = toWire(blockPlain);
    constexpr std::uint32_t kChunkSize = 128;
    for (std::uint32_t offset = 0; offset < kWindowLength; offset += kChunkSize)
    {
        const std::uint32_t addr = kWindowStart + offset;
        bytes::Bytes req = bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr),
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        // Content is never even inspected by the executor for this
        // exchange -- script a deliberately wrong reply to prove that.
        transport.exchange(request(req), response({0x7F, 0xB6, 0x99}));
    }

    // Close: wrong content, still non-fatal.
    transport.exchange(request({0x37}), response({0x7F, 0x37, 0x22}));

    // Checksum: wrong content, still non-fatal.
    transport.exchange(request({0x31, 0x01, 0x02, 0x02, 0x01}), response({0x7F, 0x31, 0x22}));

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
    EXPECT_THAT(clock.sleep_calls, testing::Contains(8000ms));
    EXPECT_THAT(clock.sleep_calls, testing::Contains(5000ms));
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, WriteStopsOnATimeoutBetweenChunks)
{
    // The per-chunk 0xB6 write exchange is presence-blind too (legacy never
    // inspects the reply at all), so this test targets the setup exchange
    // instead: after 6 timed-out attempts, reflash_block's setup proceeds
    // regardless (matching the retry-loop's own "proceed regardless"
    // shape) -- so the fatal timeout in this test is scripted at the FIRST
    // write chunk send/receive itself, a hard Disconnected during the
    // per-chunk read, proving a genuine transport failure (not just an
    // absent reply) still stops the write.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptEraseMemory(transport);

    transport.exchange(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                bytes::u24(kWindowStart), bytes::u24(kWindowLength))),
                       response({0x74}));

    const bytes::ByteView blockPlain = bytes::ByteView(rom).subspan(kWindowStart, kWindowLength);
    const bytes::Bytes encrypted = toWire(blockPlain);
    bytes::Bytes firstChunkReq =
        bytes::composeBe(bytes::Byte(0xB6), bytes::u24(kWindowStart), bytes::ByteView(encrypted).subspan(0, 128));
    transport.exchange(request(firstChunkReq));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone mid-write");

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;

    auto plan = handBuiltPlan(FlashOperation::Write, kImageSize - 1);

    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::InvalidConfig, HasSubstr("0x80000")));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_THAT(events.logs, IsEmpty());
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct TcuCvtMitsuMh8104CanTraits
{
    using Executor = SubaruTcuCvtMitsuMh8104CanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = kWindowStart;
    static constexpr std::uint32_t kBlockLength = kWindowLength;
    static constexpr std::uint32_t kPageSize = 0x100;
    // Every exchange in this family (connect and read alike) is a single_shot
    // call at this one literal 200ms timeout -- there is no separate "probe"
    // policy.
    static constexpr std::chrono::milliseconds kProbeTimeout{200};
    static constexpr int kProbeCount = 1930;

    static fastecu::Result<fastecu::flash::FlashPlan> readPlan()
    {
        return ::readPlan();
    }

    static fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation)
    {
        return ::handBuiltPlan(operation, kImageSize);
    }

    static void scriptBenchConnect(ScriptedCanFlashTransport& t)
    {
        ::scriptFullConnect(t);
    }

    static void scriptReadSetup(ScriptedCanFlashTransport& t)
    {
        ::scriptDumpSetup(t);
    }

    static void scriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length,
                                std::uint32_t pagesize, bytes::Byte fill)
    {
        ::scriptFlashDump(t, start, length, pagesize, fill);
    }

    static void scriptStopCommand(ScriptedCanFlashTransport& t)
    {
        ::scriptStopCommand(t);
    }

    static void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
    {
        ::scriptUpToFirstFatalRead(t);
    }
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruTcuCvtMitsuMh8104Can, CanExecutorConformance, TcuCvtMitsuMh8104CanTraits);

} // namespace
