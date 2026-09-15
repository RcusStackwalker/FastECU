// Equivalence tests for SubaruHitachiM32rCanExecutor, the portable replacement
// for flash_ecu_subaru_hitachi_m32r_can_operation.cpp.
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
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
using fastecu::flash::build_subaru_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruHitachiM32rCanExecutor;
using fastecu::flash::SubaruHitachiM32rCanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_ecu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB_1block";

// Every request carries the 4-byte big-endian 0x7E0 envelope; every response
// the 0x7E8 reply id (legacy connect_bootloader() of
// flash_ecu_subaru_hitachi_m32r_can_operation.cpp).
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e0);
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
    bytes::appendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    return response(bytes::ByteView(tail.begin(), tail.size()));
}

fastecu::Result<fastecu::flash::FlashPlan> readPlan()
{
    return build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> writePlan(bytes::Bytes rom)
{
    return build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_hitachi_m32r_can_plan, so a
// plan whose image size or operation the builder itself would refuse can
// still reach the executor -- the only way to prove the executor's own
// validate_subaru_hitachi_m32r_can_plan call (not just the builder) rejects
// it before any I/O.
fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation, std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruHitachiM32rCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{0, 0x80000};
    fields.erase_regions = {fastecu::flash::MemoryRegion{0, 0x80000}};
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = SubaruHitachiM32rCanPlan{0x7e0, 0x7e8, 500000, false};
    return fastecu::flash::validate_and_build(std::move(fields));
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload) and the same 32-byte indextransformation table shared by
// every family in this wave -- not read back from the executor's own
// translation unit, so a wrong table entry in the executor fails these
// assertions instead of passing silently. Mirrors
// mitsu_colt_m32r_can_executor_test.cpp's own `MitsuColtCan::seedKey(kSeed)`.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x90A1, 0x2F92, 0xDE3C, 0xCDC0, 0x1A99, 0x437C, 0xF91B, 0xDB57,
                                                      0x96BA, 0xDE10, 0xFCAF, 0x3F31, 0xF47F, 0x0BB6, 0x16E9, 0x4645};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x14CA, 0x77F4, 0x973C, 0xF50E};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes seedKey(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

// The encrypt table is a genuine round-trip inverse of the decrypt table the
// executor applies to read data (SsmProtocol::calculatePayload's Feistel
// structure inverts by reversing key order, and kDecryptTable in the
// executor is kEncryptTable exactly reversed), so this single helper both
// (a) pre-encrypts a known plaintext into the wire bytes a scripted read
// reply must carry for the executor's decrypt step to recover it, and (b)
// computes the wire bytes a write must carry for a known plaintext image.
bytes::Bytes toWire(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptTable,
                                         kIndexTransformation);
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// The OBK-probe-miss + four non-fatal identity queries: every one of these is
// scripted with a valid, if uninteresting, reply so the non-fatal path falls
// straight through regardless of content.
void scriptPreliminaryProbes(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("preliminary probes");
    transport.exchange(request({0xB7}), response({0x7F, 0xB7, 0x11}));

    transport.exchange(request({0xAA}), response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.exchange(request({0x09, 0x02}), response({0x49, 0x02, 'V', 'I', 'N'}));

    transport.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));

    transport.exchange(request({0x09, 0x06}), response({0x49, 0x06, 0xAA, 0xBB}));
}

// Scripts the full bench-branch connect sequence: the preliminary probes
// above, the session-scope probe selecting the bench arm, session, seed/key,
// jump-to-kernel, and the alive check (legacy connect_bootloader).
void scriptBenchConnect(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("bench connect");
    scriptPreliminaryProbes(transport);

    // Session-scope probe: 0xA8 0x00 0x00 0x00 0xD7. Response at[1]==0xA0
    // and/or at[2]==0x20 selects the bench branch.
    transport.exchange(request({0xA8, 0x00, 0x00, 0x00, 0xD7}), response({0x00, 0xA0, 0x20}));

    // Bench branch: session 0x10 0x43 / 0x50 0x43.
    transport.exchange(request({0x10, 0x43}), response({0x50, 0x43}));

    // Seed request: 0x27 0x01 / 0x67 0x01 <4-byte seed>.
    transport.exchange(request({0x27, 0x01}), response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));

    // Seed key: 0x27 0x02 <4-byte key>.
    bytes::Bytes keyRequest{0x27, 0x02};
    const bytes::Bytes key = seedKey(kSeed);
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.exchange(request(keyRequest), response({0x67, 0x02}));

    // Jump to kernel: 0x10 0x42 / 0x50 0x42.
    transport.exchange(request({0x10, 0x42}), response({0x50, 0x42}));

    // Kernel-alive check: 0x34 0x04 0x33 0x00 0x00 0x00 0x08 0x00 0x00 /
    // 0x74 0x20 0x01 0x04.
    transport.exchange(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       response({0x74, 0x20, 0x01, 0x04}));
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem).
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("dump setup");
    transport.exchange(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       response({0x75, 0x20, 0x01, 0x01}));
}

// Scripts the chunked 0xB7 dump sweep over [start, start+length) at
// `pagesize`-byte pages, each page filled with `fill` (plaintext -- the
// scripted wire bytes are toWire(fill-page), decrypted back by the executor).
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

// Scripts the "Sending stop command..." exchange (legacy read_mem).
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("stop command");
    transport.exchange(request({0x37}), response({0x77}));
}

// Connect plus the full (non-fatal) dump-setup exchange plus the first 0xB7
// dump-chunk request, registered as an expected write only -- its reply is
// left for the caller to queue, so the same script serves every "the next
// read fails" conformance test (fastecu::flash::testing::CanExecutorConformance)
// regardless of which failure mode (a transport error, a bare timeout, or an
// empty frame) belongs there. Unlike its siblings, this family's own dump-
// setup exchange is non-fatal on a mismatch or empty reply (dump_flash_range
// only logs and carries on) -- the first genuinely fatal read in the whole
// read path is the first dump-chunk request itself, inside the same `for`
// loop that carries the sweep's cancellation check and progress
// accumulation, so there is no shallower cut point available for this
// family.
void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& transport)
{
    scriptBenchConnect(transport);
    scriptDumpSetup(transport);
    const auto section = transport.section("first dump chunk (request only)");
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(0))));
}

// Scripts erase_memory's single write + single successful read.
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("erase memory");
    transport.exchange(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}), response({0x71, 0x01, 0x02}));
}

// Scripts reflash_block's "Setting flash start & length..." exchange.
void scriptReflashSetup(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("reflash setup");
    transport.exchange(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}), response({0x74}));
}

// Scripts the 0xB6 write-chunk sweep for the whole ROM (legacy reflash_block).
// `rom` is encrypted once, matching production.
void scriptReflashChunks(ScriptedCanFlashTransport& transport, bytes::ByteView rom, std::uint32_t chunkSize)
{
    const auto section = transport.section("reflash chunks");
    const bytes::Bytes encrypted = toWire(rom);
    for (std::uint32_t addr = 0; addr < rom.size(); addr += chunkSize)
    {
        bytes::Bytes req =
            bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr), bytes::ByteView(encrypted).subspan(addr, chunkSize));
        transport.exchange(request(req), response({0xF6}));
    }
}

// Scripts one close-block attempt (0x37) with the given tail. A tail of
// {0x77} succeeds; anything else is the tolerant-retry loop's "not yet".
void scriptCloseAttempt(ScriptedCanFlashTransport& transport, std::initializer_list<bytes::Byte> tail)
{
    const auto section = transport.section("close attempt");
    transport.exchange(request({0x37}), response(tail));
}

// Scripts the checksum-verify exchange: UdsClient absorbs the intermediate
// 0x78 (responsePending) NRC by re-reading, so only one write is expected even
// though two reads are queued.
void scriptChecksumVerify(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.section("checksum verify");
    transport.exchange(request({0x31, 0x01, 0x02, 0x02, 0x01}), response({0x7F, 0x31, 0x78}));
    transport.queueRead(response({0x71, 0x01, 0x02}));
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

TEST(SubaruHitachiM32rCanExecutor, ConnectAndReadReturnsTheFullRomFromAddressZero)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptBenchConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0, 0x80000, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), 0x80000U);
    EXPECT_TRUE(std::ranges::all_of(*result->read_bytes, [](bytes::Byte b) { return b == 0x5A; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, ConnectRejectsOnCarProgrammingAsUnsupported)
{
    // Session-scope probe response with neither at[1]==0xA0 nor at[2]==0x20
    // selects the on-car branch, which this port deliberately does not
    // implement -- see the design's on-car scope decision and docs/flash-
    // qualification-matrix.md's FlashEcuSubaruHitachiM32rCan row.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptPreliminaryProbes(transport);
    transport.exchange(request({0xA8, 0x00, 0x00, 0x00, 0xD7}), response({0x00, 0x00, 0x00}));

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Unsupported));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, ReadStopsWhenCancelledBeforeAnyExchange)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    cancellation.cancel();

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruHitachiM32rCanExecutor, WriteErasesAndWritesTheFullRomInOneReflashBlock)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptBenchConnect(transport);
    // Legacy write_mem calls erase_memory() before the single reflash_block()
    // call -- the plan and design doc's task-1 brief omits this step, but both
    // the actual legacy source and the design spec's "Portable contract"
    // section (`0x31` RoutineControl erase `0x02 0x01`) confirm it happens;
    // ported faithfully here.
    scriptEraseMemory(transport);
    scriptReflashSetup(transport);
    scriptReflashChunks(transport, rom, 256);
    scriptCloseAttempt(transport, {0x77});
    scriptChecksumVerify(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
}

TEST(SubaruHitachiM32rCanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;

    // build_subaru_hitachi_m32r_can_plan rejects this image, but
    // validate_and_build does not. The executor must still reject it before
    // it configures or opens the transport, let alone reaches the ECU
    // handshake.
    auto plan = handBuiltPlan(FlashOperation::Write, 0x7ffff);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::InvalidConfig, HasSubstr("0x80000")));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruHitachiM32rCanExecutor, WriteToleratesUpToFiveFailedCloseAttemptsBeforeSucceeding)
{
    // Legacy reflash_block's close-block loop retries up to 6 times and
    // proceeds to checksum verification even if every attempt reports
    // something other than 0x77 (the loop's `connected` flag is read nowhere
    // after the loop). Scripts 5 non-0x77 responses followed by a 6th 0x77,
    // and asserts overall success -- pinning the retry-tolerant quirk
    // explicitly.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    scriptReflashSetup(transport);
    scriptReflashChunks(transport, rom, 256);
    for (int i = 0; i < 5; ++i)
    {
        scriptCloseAttempt(transport, {0x7F, 0x37, 0x22});
    }
    scriptCloseAttempt(transport, {0x77});
    scriptChecksumVerify(transport);

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, WriteStopsWhenTheEraseIsRejected)
{
    // Despite the name (matching the brief's Step 10 list), this pins a
    // negative response on the FIRST 0xB6 write chunk, not a separate erase
    // step -- this family has no distinct "erase a block" exchange beyond
    // erase_memory(), which is scripted (and succeeds) before reaching here.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    scriptReflashSetup(transport);

    const bytes::Bytes encrypted = toWire(rom);
    bytes::Bytes firstChunkRequest =
        bytes::composeBe(bytes::Byte(0xB6), bytes::u24(0), bytes::ByteView(encrypted).subspan(0, 256));
    transport.exchange(request(firstChunkRequest), response({0x7F, 0xB6, 0x22}));

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct HitachiM32rCanTraits
{
    using Executor = SubaruHitachiM32rCanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = 0;
    static constexpr std::uint32_t kBlockLength = 0x80000;
    static constexpr std::uint32_t kPageSize = 0x100;
    // The single ExchangePolicy (kExchangePolicy in the executor) every
    // connect and read exchange uses -- there is no separate "probe" timeout
    // distinct from the rest of the read path for this family.
    static constexpr std::chrono::milliseconds kProbeTimeout{500};
    static constexpr int kProbeCount = 2059;

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
        ::scriptBenchConnect(t);
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

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruHitachiM32rCan, CanExecutorConformance, HitachiM32rCanTraits);

} // namespace
