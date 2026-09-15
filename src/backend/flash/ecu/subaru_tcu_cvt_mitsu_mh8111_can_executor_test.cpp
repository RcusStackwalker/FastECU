// Equivalence tests for SubaruTcuCvtMitsuMh8111CanExecutor, the portable
// replacement for flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp.
//
// Two disclosed deliberate divergences from the literal legacy source: (1)
// erase_mem's own retry loop never resends and never breaks on success in
// legacy (so write_mem always failed at the erase step in production) -- this
// ports the evident retry-until-match intent shared by every sibling retry
// loop in the same file instead; (2) the "alive check" exchange in
// connect_bootloader sends a 0x34-prefixed PDU but checks for a 0x71-prefixed
// reply (a different service's SID+0x40), which the brief's own Step 7
// description got wrong (it describes Task 3's alive-recheck bytes instead) --
// the executor uses the bytes actually found in the legacy source.
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.h"

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
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
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
using fastecu::flash::build_subaru_tcu_cvt_mitsu_mh8111_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanExecutor;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8111_can";
constexpr std::string_view kMcu = "MH8111";
constexpr std::uint32_t kReadStart = 0x8000;
constexpr std::uint32_t kReadLength = 0x78000;
constexpr std::uint32_t kWriteStart = 0x80000;
constexpr std::uint32_t kWriteLength = 0x100000;
constexpr std::uint32_t kImageSize = 0x180000;

// This family's own request/reply envelope -- every exchange (unlike Task
// 3's Hitachi CAN sibling) is sent on 0x7e1/0x7e9; there is no second id.
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
    return build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> writePlan(bytes::Bytes rom)
{
    return build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_tcu_cvt_mitsu_mh8111_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_mitsu_mh8111_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation, std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruTcuCvtMitsuMh8111Can;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kWriteStart, kWriteLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kWriteStart, kWriteLength}};
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = SubaruTcuCvtMitsuMh8111CanPlan{0x7e1, 0x7e9, 500000, false};
    return fastecu::flash::validate_and_build(std::move(fields));
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload) -- not read back from the executor's own translation unit.
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

// TCU ID (0xAA) / CAL ID (0x09/0x04): both non-fatal, scripted with a valid-
// if-uninteresting reply.
void scriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("identity queries");
    transport.exchange(request({0xAA}), response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session 0x10/0x43, non-fatal.
void scriptSession(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("session");
    transport.exchange(request({0x10, 0x43}), response({0x50, 0x43}));
}

// Seed (0x27/0x01) and seed key (0x27/0x02), both fatal.
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

// Jump 0x10/0x42, fatal.
void scriptJump(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("jump");
    transport.exchange(request({0x10, 0x42}), response({0x50, 0x42}));
}

// Alive check: sent 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/ 0x00, checked
// against a 0x71-prefixed reply -- a different service's SID+0x40, not this
// PDU's own (0x74). Confirmed directly against the legacy source (re-read
// twice); see the file header comment.
void scriptAliveCheck(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("alive check");
    transport.exchange(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       response({0x71, 0x02, 0x02, 0x03}));
}

// Scripts the full 7-exchange connect_bootloader sequence -- there is no
// kernel-alive pre-check shortcut for this family (unlike Task 3's Hitachi
// CAN sibling), so this always runs in full.
void scriptFullConnect(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("full connect");
    scriptIdentityQueries(transport);
    scriptSession(transport);
    const bytes::Bytes key = seedKey(kSeed);
    scriptSeedAndKey(transport, kSeed, key);
    scriptJump(transport);
    scriptAliveCheck(transport);
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem):
// sent 0x35-prefixed, checked against a 0x74-prefixed reply -- again a service
// mismatch, confirmed directly against source.
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("dump setup");
    transport.exchange(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
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

// Scripts the "Sending stop command..." exchange (legacy read_mem): content-
// blind, succeeding on the first non-empty reply.
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("stop command");
    transport.exchange(request({0x37}), response({0x77}));
}

// Identity queries, session, and the seed request, registered as an expected
// write only -- its reply is left for the caller to queue, so the same
// script serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there.
//
// Deliberately NOT cut at read_mem's own 0x35 dump-setup exchange, unlike the
// sibling MH8104 family: that exchange is routed through ctx.channel
// directly and classifies an absent reply as ErrorKind::BadResponse ("dump
// start & length setup rejected"), not Timeout -- a different assertion,
// not just a different constant, from what the shared
// ReadReportsAnEmptyReplyAsTimeout test checks. The seed request, by
// contrast, goes through the standard fatal_query/UdsClient::request path,
// which maps every one of these three failure modes exactly the way the
// conformance suite expects, so it is the cut point used here instead.
void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& transport)
{
    scriptIdentityQueries(transport);
    scriptSession(transport);
    const auto section = transport.section("seed (first request only)");
    transport.exchange(request({0x27, 0x01}));
}

// Scripts erase_mem's single successful attempt (-- see the file header
// comment on the evident retry-until-match intent this ports instead of the
// literal always-failing loop).
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("erase memory");
    transport.exchange(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}), response({0x71, 0x01, 0x02}));
}

// Scripts unlock_and_reflash_block's setup, 256-byte chunk sweep (content-
// blind), close and checksum, each succeeding on the first attempt.
void scriptWriteBlock(ScriptedCanFlashTransport& transport, bytes::ByteView blockPlain)
{
    const auto section = transport.section("write block");
    constexpr std::uint32_t kChunkSize = 256;
    // Legacy's own halved data_len bug (maxblocks*128 instead of *256, see
    // reflash_block's comment): kWriteLength/256 * 128 == kWriteLength/2.
    constexpr std::uint32_t kSetupDataLen = (kWriteLength / kChunkSize) * 128;

    transport.exchange(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33), bytes::u24(0),
                                                bytes::u24(kSetupDataLen))),
                       response({0x74}));

    const bytes::Bytes encrypted = toWire(blockPlain);
    for (std::uint32_t offset = 0; offset < kWriteLength; offset += kChunkSize)
    {
        const std::uint32_t addr = kWriteStart + offset;
        bytes::Bytes req = bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr),
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        transport.exchange(request(req), response({0xF6}));
    }

    transport.exchange(request({0x37}), response({0x77}));

    transport.exchange(request({0x31, 0x01, 0x02, 0x02, 0x01}), response({0x71, 0x01, 0x02}));
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

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ConnectFullSequenceEveryTime)
{
    // Proves there is no alive-skip shortcut (unlike Task 3): scripts every
    // one of the 7 connect_bootloader exchanges in exact order. The fake
    // does byte-exact matching, so a skipped or reordered exchange would
    // fail the very next write comparison instead of silently passing.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadReturnsTheLowerWindowPaddedWithFF)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kReadStart + kReadLength);
    // Padded with 0xFF, NOT 0x00 -- this family differs from Task 3's
    // Hitachi CAN sibling here.
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->begin() + kReadStart,
                            [](bytes::Byte b) { return b == 0xFF; }));
    EXPECT_TRUE(std::all_of(result->read_bytes->begin() + kReadStart, result->read_bytes->end(),
                            [](bytes::Byte b) { return b == 0x5A; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    cancellation.cancel();

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_mem's 0x35 dump-setup exchange -- routed through
// ctx.channel directly but still a single, non-looped call -- this pins a
// transport error raised *inside* the 0xB7 dump-chunk loop, whose reads go
// through fatal_request at a different call site, inside a `for` loop
// carrying its own cancellation-check and progress-accumulation state.
// fatal_request's generic error propagation is covered once for every family
// by uds_client_exchange_common_test.cpp; this test is what actually proves
// the loop itself aborts cleanly -- without corrupting rom/progress state --
// on a transport error, rather than assuming fatal_request's coverage
// implies the loop wrapping it behaves the same way.
TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(kReadStart))));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteErasesThenFlashesTheUpperBlockAtAddressAbove0x80000)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptFullConnect(transport);
    scriptEraseMemory(transport);
    scriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength));

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;

    // build_subaru_tcu_cvt_mitsu_mh8111_can_plan rejects this image, but
    // validate_and_build does not. The executor must still reject it before
    // it reaches the TCU handshake.
    auto plan = handBuiltPlan(FlashOperation::Write, kImageSize - 1);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::InvalidConfig, HasSubstr("0x180000")));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_THAT(events.logs, IsEmpty());
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct TcuCvtMitsuMh8111CanTraits
{
    using Executor = SubaruTcuCvtMitsuMh8111CanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = kReadStart;
    static constexpr std::uint32_t kBlockLength = kReadLength;
    static constexpr std::uint32_t kPageSize = 0x100;
    // Every exchange in this family's connect + read path (kExchangePolicy's
    // read_timeout, and the alive-check/dump-setup/dump-chunk reads that
    // share its literal 2000ms value) uses this one timeout -- only the
    // stop-command retry loop (800ms) differs, so it is excluded from the
    // count below.
    static constexpr std::chrono::milliseconds kProbeTimeout{2000};
    // The 1928 breaks down as: TCU ID + CAL ID + session + seed + seed key +
    // jump + alive check + dump setup (8) plus one per dump chunk
    // (kBlockLength / kPageSize == 1920).
    static constexpr int kProbeCount = 1928;

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

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruTcuCvtMitsuMh8111Can, CanExecutorConformance, TcuCvtMitsuMh8111CanTraits);

} // namespace
