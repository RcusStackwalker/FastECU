// Equivalence tests for SubaruTcuCvtMitsuMh8104CanExecutor, the portable
// replacement for FlashTcuCvtSubaruMitsuMH8104CanOperation's
// connect_bootloader(), read_mem(), write_mem(), reflash_block() and
// erase_mem(). Expected wire bytes are transcribed character-for-character
// from
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp.
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
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_tcu_cvt_mitsu_mh8104_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruTcuCvtMitsuMh8104CanExecutor;
using fastecu::flash::SubaruTcuCvtMitsuMh8104CanPlan;
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

fastecu::flash::FlashPlan readPlan()
{
    auto plan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_tcu_cvt_mitsu_mh8104_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_mitsu_mh8104_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation, std::size_t image_size)
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
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload, lines 899-965) -- not read back from the executor's own
// translation unit. Identical to the sibling MH8111 family's own tables,
// re-confirmed against MH8104's own legacy source directly, not assumed.
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

// Scripts the alive probe (0x31/0x02/0x02/0x01, lines 106-127) with a MISS
// reply -- present but not matching "already running" -- so connect falls
// through into the full init sequence.
void scriptAliveProbeMiss(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x71, 0x00, 0x00, 0x00}));
}

// TCU ID 0xAA (lines 129-157) / CAL ID 0x09/0x04 (lines 171-197): both
// retried up to 6 times, content-blind -- scripted with a single successful
// reply each since the executor stops retrying on the first non-empty one.
void scriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0xAA}));
    transport.queueRead(response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.expectWrite(request({0x09, 0x04}));
    transport.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session 0x10/0x43 (lines 205-226), content-blind.
void scriptSession(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));
}

// Seed (0x27/0x01) and seed key (0x27/0x02), lines 228-282, both
// content-blind (single-shot, fatal only on a genuine transport failure).
void scriptSeedAndKey(ScriptedCanFlashTransport& transport, bytes::ByteView seed, bytes::ByteView key)
{
    transport.expectWrite(request({0x27, 0x01}));
    bytes::Bytes seedResponse{0x67, 0x01};
    seedResponse.insert(seedResponse.end(), seed.begin(), seed.end());
    transport.queueRead(response(seedResponse));

    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(request(keyRequest));
    transport.queueRead(response({0x67, 0x02}));
}

// Jump 0x10/0x42, lines 287-307, content-blind.
void scriptJump(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x10, 0x42}));
    transport.queueRead(response({0x50, 0x42}));
}

// Alive re-check (lines 312-343): sent 0x34/0x04/0x33/0x00/0x00/0x00/0x08/
// 0x00/0x00, checked with legacy's buggy `&&` condition against
// 0x74/0x20/0x01/0x04 -- scripted here with the "expected" reply
// (0x74/0x20/0x01/0x04), which per the literal `&&` bug does NOT log
// "Kernel verified to be running" (since none of the four bytes differ),
// but still succeeds unconditionally either way.
void scriptAliveRecheck(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x74, 0x20, 0x01, 0x04}));
}

// Scripts the full connect_bootloader sequence past a missed alive probe.
void scriptFullConnect(ScriptedCanFlashTransport& transport)
{
    scriptAliveProbeMiss(transport);
    scriptIdentityQueries(transport);
    scriptSession(transport);
    const bytes::Bytes key = seedKey(kSeed);
    scriptSeedAndKey(transport, kSeed, key);
    scriptJump(transport);
    scriptAliveRecheck(transport);
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem,
// lines 373-401): sent 0x35-prefixed, checked against a 0x75-prefixed
// reply, content-blind.
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x75, 0x20, 0x01, 0x01}));
}

// Scripts the chunked 0xB7 dump sweep over [start, start+length) at
// `pagesize`-byte pages, each page filled with `fill` (plaintext -- the
// scripted wire bytes are toWire(fill-page), decrypted back by the
// executor).
void scriptFlashDump(ScriptedCanFlashTransport& transport, std::uint32_t start, std::uint32_t length,
                     std::uint32_t pagesize, bytes::Byte fill)
{
    const bytes::Bytes plainPage(pagesize, fill);
    const bytes::Bytes wirePage = toWire(plainPage);
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        transport.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(addr))));
        bytes::Bytes reply = response({0xF7});
        reply.insert(reply.end(), wirePage.begin(), wirePage.end());
        transport.queueRead(reply);
    }
}

// Scripts the "Sending stop command..." exchange (legacy read_mem, lines
// 505-532): content-blind, succeeding on the first non-empty reply.
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x77}));
}

// Scripts erase_mem's single exchange (legacy lines 847-892): NOT a retry
// loop, unlike the sibling MH8111 family's own erase.
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(response({0x71, 0x01, 0x02}));
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
        rom[i] = static_cast<bytes::Byte>(i & 0xff);
    }
    return rom;
}

// Records every ctx.clock.sleep() call's ms argument, so the erase step's
// 8000ms/5000ms delays (legacy lines 877/883) can be asserted without a
// real multi-second wait.
class RecordingClock final : public FakeClock
{
  public:
    fastecu::Status sleep(int ms, const fastecu::ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(ms);
        return FakeClock::sleep(ms, cancellation);
    }
    std::vector<int> sleep_calls;
};

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, TransportSetupReturnsThePlansWireParameters)
{
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    const auto plan = readPlan();

    const auto setup = executor.transport_setup(plan);

    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e1U);
    EXPECT_EQ(setup->response_id, 0x7e9U);
    EXPECT_FALSE(setup->extended_id);
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;

    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = fastecu::flash::FlashFamily::DensoSh705xEepromCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = fastecu::flash::MemoryRegion{.start = 0x0, .length = 0x100};
    fields.kernel = fastecu::flash::KernelImage{.id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}};
    fields.family_plan = fastecu::flash::DensoSh705xEepromCanPlan{
        .mode = fastecu::flash::EepromReadMode::Mode2,
        .security = fastecu::flash::DensoSecurityVariant::Stock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };
    auto foreign = fastecu::flash::validate_and_build(std::move(fields));
    ASSERT_TRUE(foreign.has_value()) << foreign.error().detail;

    const auto result = executor.execute(*foreign, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("does not match this executor"));
    EXPECT_THAT(events.logs, IsEmpty());
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ConnectSkipsTheRestWhenKernelAlreadyRunning)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request({0x31, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x71, 0x02, 0x02, 0x03}));
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, kWindowLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
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

    scriptAliveProbeMiss(transport);

    transport.expectWrite(request({0xAA}));
    transport.queueRead(response({0x7F, 0xAA, 0x11})); // negative response, still "a reply"

    transport.expectWrite(request({0x09, 0x04}));
    transport.queueRead(response({0x7F, 0x09, 0x11}));

    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x7F, 0x10, 0x22})); // wrong content

    transport.expectWrite(request({0x27, 0x01}));
    // Still shaped like a seed reply so a key can be computed, but wrong
    // SID -- this family does not validate the SID before using the bytes.
    // The executor extracts the 4 seed bytes at stripped-PDU offsets [2..5]
    // (uds::payload(pdu)[1..4], mirroring legacy's received.at(6..9)), i.e.
    // {0x33, 0xAA, 0xBB, 0xCC} here, NOT the trailing 4 bytes of this
    // 7-byte reply -- legacy indexes by absolute position, not by "the
    // seed field of a well-formed 0x67/0x01 reply".
    transport.queueRead(response({0x7F, 0x27, 0x33, 0xAA, 0xBB, 0xCC, 0xDD}));

    const bytes::Bytes seed{0x33, 0xAA, 0xBB, 0xCC};
    const bytes::Bytes key = seedKey(seed);
    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(request(keyRequest));
    transport.queueRead(response({0x7F, 0x27, 0x35}));

    transport.expectWrite(request({0x10, 0x42}));
    transport.queueRead(response({0x7F, 0x10, 0x22}));

    transport.expectWrite(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x7F, 0x34, 0x11}));

    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, kWindowLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
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

    scriptAliveProbeMiss(transport);
    scriptIdentityQueries(transport);
    scriptSession(transport);

    transport.expectWrite(request({0x27, 0x01}));
    bytes::Bytes seedResponse{0x67, 0x01};
    seedResponse.insert(seedResponse.end(), kSeed.begin(), kSeed.end());
    transport.queueRead(response(seedResponse));

    const bytes::Bytes key = seedKey(kSeed);
    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(request(keyRequest));
    transport.queue_no_frame();

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
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

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, kWindowLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
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
    cancellation.cancel();

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

// Cancels the token as soon as the first dump chunk's progress is
// reported, mirroring Task 1/3/4's own CancelAfterFirstChunkSink pattern.
class CancelAfterFirstChunkSink final : public RecordingEventSink
{
  public:
    explicit CancelAfterFirstChunkSink(fastecu::ManualCancellationToken& source) : source_(source)
    {
    }
    void phase_progress(const fastecu::PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == "Read ROM" && event.done > 0)
        {
            source_.cancel();
        }
    }

  private:
    fastecu::ManualCancellationToken& source_;
};

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    // Exercises the cancellation check at the top of dump_flash_range's
    // page loop (legacy stopRequested(), line 423): connect and the first
    // 0x100 dump chunk are scripted, cancel() lands on that chunk's
    // progress event, and the loop must stop before requesting a second
    // chunk -- there is no second chunk scripted, so any further write
    // would fail against the exhausted script instead.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    CancelAfterFirstChunkSink events{cancellation};
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kWindowStart, 0x100, 0x100, 0x5A);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, ReadPropagatesADisconnectedTransport)
{
    // A transport-level Disconnected failure mid-read must surface as
    // ErrorKind::Disconnected, not be swallowed or misclassified as a
    // malformed/timeout response -- distinguishing a hard transport fault
    // from a merely-wrong ECU reply, which this family tolerates.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(kWindowStart))));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
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

    scriptFullConnect(transport);

    // Erase (lines 847-892): scripted with a deliberately WRONG content
    // reply -- still succeeds (non-fatal), and the 8000ms/5000ms delays
    // are asserted via RecordingClock below, not a real wait.
    transport.expectWrite(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(response({0x7F, 0x31, 0x22}));

    // reflash_block setup (lines 677-716): the retry loop is content-blind
    // -- ANY reply, right or wrong, stops it after the FIRST attempt (it is
    // presence, not content, that ends the retry), so only one attempt is
    // scripted here, with a deliberately wrong reply.
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                   bytes::u24(kWindowStart), bytes::u24(kWindowLength))));
    transport.queueRead(response({0x7F, 0x34, 0x22}));

    const bytes::ByteView blockPlain = bytes::ByteView(rom).subspan(kWindowStart, kWindowLength);
    const bytes::Bytes encrypted = toWire(blockPlain);
    constexpr std::uint32_t kChunkSize = 128;
    for (std::uint32_t offset = 0; offset < kWindowLength; offset += kChunkSize)
    {
        const std::uint32_t addr = kWindowStart + offset;
        bytes::Bytes req = bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr),
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        transport.expectWrite(request(req));
        // Content is never even inspected by the executor for this
        // exchange -- script a deliberately wrong reply to prove that.
        transport.queueRead(response({0x7F, 0xB6, 0x99}));
    }

    // Close: wrong content, still non-fatal.
    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x7F, 0x37, 0x22}));

    // Checksum: wrong content, still non-fatal.
    transport.expectWrite(request({0x31, 0x01, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x7F, 0x31, 0x22}));

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
    EXPECT_THAT(clock.sleep_calls, testing::Contains(8000));
    EXPECT_THAT(clock.sleep_calls, testing::Contains(5000));
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

    scriptFullConnect(transport);
    scriptEraseMemory(transport);

    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                   bytes::u24(kWindowStart), bytes::u24(kWindowLength))));
    transport.queueRead(response({0x74}));

    const bytes::ByteView blockPlain = bytes::ByteView(rom).subspan(kWindowStart, kWindowLength);
    const bytes::Bytes encrypted = toWire(blockPlain);
    bytes::Bytes firstChunkReq =
        bytes::composeBe(bytes::Byte(0xB6), bytes::u24(kWindowStart), bytes::ByteView(encrypted).subspan(0, 128));
    transport.expectWrite(request(firstChunkReq));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone mid-write");

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
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

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("0x80000"));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruTcuCvtMitsuMh8104CanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    // cfg test_write=no for this family; build_subaru_tcu_cvt_mitsu_mh8104_can_plan
    // rejects TestWrite outright (Step 5's plan code), so this pins the
    // executor's own repeated guard using a hand-built plan that bypasses
    // the builder -- there is no connect handshake to script here.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8104CanExecutor executor;
    auto plan = handBuiltPlan(FlashOperation::TestWrite, kImageSize);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

} // namespace
