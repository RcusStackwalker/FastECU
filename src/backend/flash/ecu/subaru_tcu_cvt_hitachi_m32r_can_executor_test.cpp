// Equivalence tests for SubaruTcuCvtHitachiM32rCanExecutor, the portable
// replacement for flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp.
//
// The ported logic is the REAL connect_bootloader/read_mem/write_mem/
// reflash_block/erase_mem that legacy execute() never reached -- it called the
// always-failing hack_words() instead.
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_tcu_cvt_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruTcuCvtHitachiM32rCanExecutor;
using fastecu::flash::SubaruTcuCvtHitachiM32rCanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_tcu_cvt_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB";

// This family's own request/reply pair. TCU exchanges use 0x7e1/0x7e9,
// unlike the ECU family's 0x7e0/0x7e8.
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

// Six of connect_bootloader's exchanges are sent on 0x7E0 (the OBD
// generic-ECU arb id), not this family's own 0x7e1 -- see the executor's
// raw_exchange() comment. The executor never validates the incoming
// envelope's id for these, so the reply is still framed with 0x7e9 here
// purely for readability; any 4-byte prefix would be accepted.
bytes::Bytes requestOnId(std::uint32_t arb_id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, arb_id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes requestOnId(std::uint32_t arb_id, std::initializer_list<bytes::Byte> payload)
{
    return requestOnId(arb_id, bytes::ByteView(payload.begin(), payload.size()));
}

fastecu::Result<fastecu::flash::FlashPlan> readPlan()
{
    return build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> writePlan(bytes::Bytes rom)
{
    return build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_tcu_cvt_hitachi_m32r_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_hitachi_m32r_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation, std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruTcuCvtHitachiM32rCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{0x8000, 0x78000};
    fields.erase_regions = {fastecu::flash::MemoryRegion{0x8000, 0x78000}};
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = SubaruTcuCvtHitachiM32rCanPlan{0x7e1, 0x7e9, 500000, false};
    return fastecu::flash::validate_and_build(std::move(fields));
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload) -- not read back from the executor's own translation unit.
// Mirrors subaru_hitachi_m32r_can_executor_test.cpp's own precedent.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
                                                      0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes seedKey(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

// calculatePayload's Feistel structure inverts by reversing key order and is
// memoryless per 4-byte word (position-independent), so this single helper
// both (a) pre-encrypts a known plaintext into the wire bytes a scripted
// read reply must carry for the executor's decrypt step to recover it, and
// (b) computes the wire bytes a write must carry for a known plaintext
// image -- even for a sub-window that does not start at image offset 0.
bytes::Bytes toWire(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptTable,
                                         kIndexTransformation);
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// Kernel-alive probe, scripted as a miss (no frame at all -- legacy's "No
// valid response from ECU" branch), which falls through to full initialization
// rather than returning early.
void scriptKernelAliveMiss(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("kernel alive miss");
    transport.exchange(request({0x31, 0x02, 0x02, 0x01}));
    transport.queue_no_frame();
}

void scriptKernelAliveHit(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("kernel alive hit");
    transport.exchange(request({0x31, 0x02, 0x02, 0x01}), response({0x71, 0x02, 0x02, 0x03}));
}

// TCU ID / CAL ID queries: sent on 0x7E0, non-fatal, scripted with a valid-if-
// uninteresting reply.
void scriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("identity queries");
    transport.exchange(requestOnId(0x7e0, {0xAA}),
                       response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.exchange(requestOnId(0x7e0, {0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session (0x10/0x03 fatal, 0x10/0x43 non-fatal) and seed/seed-key exchanges:
// all sent on 0x7E0.
void scriptSessionAndSeed(ScriptedCanFlashTransport& transport, bytes::ByteView seed, bytes::ByteView key)
{
    const auto section = transport.section("session and seed");
    transport.exchange(requestOnId(0x7e0, {0x10, 0x03}), response({0x50, 0x03}));

    transport.exchange(requestOnId(0x7e0, {0x10, 0x43}), response({0x50, 0x43}));

    bytes::Bytes seedResponse{0x67, 0x01};
    seedResponse.insert(seedResponse.end(), seed.begin(), seed.end());
    transport.exchange(requestOnId(0x7e0, {0x27, 0x01}), response(seedResponse));

    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.exchange(requestOnId(0x7e0, keyRequest), response({0x67, 0x02}));
}

// Jump (0x10/0x02) and alive re-check (0x31/0x02/0x02/0x01), both back on this
// family's own 0x7e1.
void scriptJumpAndRecheck(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("jump and recheck");
    transport.exchange(request({0x10, 0x02}), response({0x50, 0x02}));

    transport.exchange(request({0x31, 0x02, 0x02, 0x01}), response({0x71, 0x02, 0x02, 0x03}));
}

// Scripts the full connect_bootloader sequence with the kernel-alive probe
// missing (the "kernel not running" branch).
void scriptFullConnect(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("full connect");
    scriptKernelAliveMiss(transport);
    scriptIdentityQueries(transport);
    const bytes::Bytes key = seedKey(kSeed);
    scriptSessionAndSeed(transport, kSeed, key);
    scriptJumpAndRecheck(transport);
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem)
// over the resolved {0x8000, 0x78000} window.
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("dump setup");
    transport.exchange(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                bytes::u24(0x8000), bytes::u24(0x78000))),
                       response({0x74, 0x20, 0x01, 0x04}));
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

// Scripts the "Sending stop command..." exchange (legacy read_mem). A well-
// formed 0x77 reply is success through fatal_request()'s standard SID+0x40
// matching -- see the executor's comment on legacy's own inverted polarity
// there.
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
// there. Unlike its non-TCU Hitachi CAN sibling, this family's dump-setup
// exchange is fatal on mismatch or empty reply, so this is a safe, shallow
// cut point.
void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& transport)
{
    scriptFullConnect(transport);
    const auto section = transport.section("dump setup (first request only)");
    transport.exchange(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                bytes::u24(0x8000), bytes::u24(0x78000))));
}

// Scripts erase_mem's single write + single successful read. The positive
// response echoes the request SID literally (0x31), not the standard SID+0x40
// convention.
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("erase memory");
    transport.exchange(request({0x31, 0x02, 0x01, 0xff, 0xff, 0xff, 0xff}), response({0x31, 0x02, 0x01}));
}

// Scripts one reflash_block block: setup, 128-byte chunk sweep (content-blind
// -- any well-formed reply is accepted), close, checksum.
void scriptWriteBlock(ScriptedCanFlashTransport& transport, bytes::ByteView blockPlain, std::uint32_t start,
                      std::uint32_t length)
{
    const auto section = transport.section("write block");
    constexpr std::uint32_t kChunkSize = 128;

    transport.exchange(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                bytes::u24(start), bytes::u24(length))),
                       response({0x74}));

    const bytes::Bytes encrypted = toWire(blockPlain);
    for (std::uint32_t offset = 0; offset < length; offset += kChunkSize)
    {
        const std::uint32_t addr = start + offset;
        bytes::Bytes req = bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr),
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        transport.exchange(request(req), response({0xF6}));
    }

    transport.exchange(request({0x37}), response({0x77}));

    transport.exchange(request({0x31, 0x02, 0x02, 0x01}), response({0x71, 0x02, 0x02}));
}

bytes::Bytes writeRom()
{
    bytes::Bytes rom(0x80000, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>(i & 0xffU);
    }
    return rom;
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ConnectSkipsTheRestWhenKernelAlreadyRunning)
{
    // : a matching alive-probe reply returns STATUS_SUCCESS immediately, with
    // zero further writes.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptKernelAliveHit(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0x8000, 0x78000, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    // Only the scripted sequence above was consumed -- if the executor had
    // continued into the rest of connect_bootloader after a matching probe
    // (identity queries, session, seed, jump...), the next write would not
    // match any of the entries scripted here and execute() would have
    // failed above instead.
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ConnectFullSequenceWhenKernelNotRunning)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0x8000, 0x78000, 0x100, 0x00);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ReadReturnsTheFloorClampedWindowPaddedWithZero)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0x8000, 0x78000, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), 0x80000U);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->begin() + 0x8000,
                            [](bytes::Byte b) { return b == 0x00; }));
    EXPECT_TRUE(std::all_of(result->read_bytes->begin() + 0x8000, result->read_bytes->end(),
                            [](bytes::Byte b) { return b == 0x5A; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    cancellation.cancel();

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_mem's dump-setup exchange -- a fatal_query call --
// this pins a transport error raised *inside* the 0xB7 dump-chunk loop, whose
// reads go through fatal_request at a different call site, inside a `for`
// loop carrying its own cancellation-check and progress-accumulation state.
// fatal_request's generic error propagation is covered once for every family
// by uds_client_exchange_common_test.cpp; this test is what actually proves
// the loop itself aborts cleanly -- without corrupting rom/progress state --
// on a transport error, rather than assuming fatal_request's coverage
// implies the loop wrapping it behaves the same way.
TEST(SubaruTcuCvtHitachiM32rCanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(0x8000))));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, WriteErasesThenFlashesEightBlocksOfSixtyFourKib)
{
    // The 8 flashed blocks (M32R_512KB indices 3-10) are NOT uniformly 64
    // KiB: block index 3 is 32 KiB, the remaining seven are 64 KiB each
    // (fblocks_M32R_512KB in kernelmemorymodels.h; the wave-3 plan's Global
    // Constraints table states 0x10000 for block 3, which does not match
    // the source -- see subaru_tcu_cvt_hitachi_m32r_can_executor.cpp's
    // kWriteBlocks comment). Scripted here with the real per-block sizes.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptEraseMemory(transport);

    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 8> kBlocks{{
        {0x08000, 0x08000},
        {0x10000, 0x10000},
        {0x20000, 0x10000},
        {0x30000, 0x10000},
        {0x40000, 0x10000},
        {0x50000, 0x10000},
        {0x60000, 0x10000},
        {0x70000, 0x10000},
    }};
    for (const auto& [start, length] : kBlocks)
    {
        scriptWriteBlock(transport, bytes::ByteView(rom).subspan(start, length), start, length);
    }

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;

    // build_subaru_tcu_cvt_hitachi_m32r_can_plan rejects this image, but
    // validate_and_build does not. The executor must still reject it before
    // it configures or opens the transport, let alone reaches the TCU
    // handshake.
    auto plan = handBuiltPlan(FlashOperation::Write, 0x7ffff);
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
struct TcuCvtHitachiM32rCanTraits
{
    using Executor = SubaruTcuCvtHitachiM32rCanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = 0x8000;
    static constexpr std::uint32_t kBlockLength = 0x78000;
    static constexpr std::uint32_t kPageSize = 0x100;
    // The single ExchangePolicy (kExchangePolicy in the executor) every
    // connect and read exchange uses.
    static constexpr std::chrono::milliseconds kProbeTimeout{2000};
    static constexpr int kProbeCount = 1930;

    static fastecu::Result<fastecu::flash::FlashPlan> readPlan()
    {
        return ::readPlan();
    }

    static fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation)
    {
        return ::handBuiltPlan(operation, 0x80000);
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

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruTcuCvtHitachiM32rCan, CanExecutorConformance, TcuCvtHitachiM32rCanTraits);

} // namespace
