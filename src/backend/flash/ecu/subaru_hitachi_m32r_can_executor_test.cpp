// Equivalence tests for SubaruHitachiM32rCanExecutor, the portable
// replacement for FlashEcuSubaruHitachiM32rCanOperation's connect_bootloader(),
// read_mem(), write_mem(), reflash_block() and erase_memory(). Expected log
// strings are copied character-for-character from
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp
// wherever the legacy class had a counterpart.
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
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

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
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_ecu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB_1block";

// Every request carries the 4-byte big-endian 0x7E0 envelope; every response
// the 0x7E8 reply id (legacy connect_bootloader(), lines 93-99 of
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

fastecu::flash::FlashPlan readPlan()
{
    auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_hitachi_m32r_can_plan, so a
// plan whose image size or operation the builder itself would refuse can
// still reach the executor -- the only way to prove the executor's own
// validate_subaru_hitachi_m32r_can_plan call (not just the builder) rejects
// it before any I/O.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation, std::size_t image_size)
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
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload, lines 1352-1422) and the same 32-byte indextransformation
// table shared by every family in this wave -- not read back from the
// executor's own translation unit, so a wrong table entry in the executor
// fails these assertions instead of passing silently. Mirrors
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

// The OBK-probe-miss + four non-fatal identity queries (legacy lines 91-257):
// every one of these is scripted with a valid, if uninteresting, reply so the
// non-fatal path falls straight through regardless of content.
void scriptPreliminaryProbes(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0xB7}));
    transport.queueRead(response({0x7F, 0xB7, 0x11}));

    transport.expectWrite(request({0xAA}));
    transport.queueRead(response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.expectWrite(request({0x09, 0x02}));
    transport.queueRead(response({0x49, 0x02, 'V', 'I', 'N'}));

    transport.expectWrite(request({0x09, 0x04}));
    transport.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));

    transport.expectWrite(request({0x09, 0x06}));
    transport.queueRead(response({0x49, 0x06, 0xAA, 0xBB}));
}

// Scripts the full bench-branch connect sequence: the preliminary probes
// above, the session-scope probe selecting the bench arm, session, seed/key,
// jump-to-kernel, and the alive check (legacy connect_bootloader lines
// 259-753).
void scriptBenchConnect(ScriptedCanFlashTransport& transport)
{
    scriptPreliminaryProbes(transport);

    // Session-scope probe: 0xA8 0x00 0x00 0x00 0xD7. Response at[1]==0xA0
    // and/or at[2]==0x20 selects the bench branch.
    transport.expectWrite(request({0xA8, 0x00, 0x00, 0x00, 0xD7}));
    transport.queueRead(response({0x00, 0xA0, 0x20}));

    // Bench branch: session 0x10 0x43 / 0x50 0x43.
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));

    // Seed request: 0x27 0x01 / 0x67 0x01 <4-byte seed>.
    transport.expectWrite(request({0x27, 0x01}));
    transport.queueRead(response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));

    // Seed key: 0x27 0x02 <4-byte key>.
    bytes::Bytes keyRequest{0x27, 0x02};
    const bytes::Bytes key = seedKey(kSeed);
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(request(keyRequest));
    transport.queueRead(response({0x67, 0x02}));

    // Jump to kernel: 0x10 0x42 / 0x50 0x42.
    transport.expectWrite(request({0x10, 0x42}));
    transport.queueRead(response({0x50, 0x42}));

    // Kernel-alive check: 0x34 0x04 0x33 0x00 0x00 0x00 0x08 0x00 0x00 /
    // 0x74 0x20 0x01 0x04.
    transport.expectWrite(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x74, 0x20, 0x01, 0x04}));
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem,
// lines 788-819).
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x75, 0x20, 0x01, 0x01}));
}

// Scripts the chunked 0xB7 dump sweep over [start, start+length) at
// `pagesize`-byte pages, each page filled with `fill` (plaintext -- the
// scripted wire bytes are toWire(fill-page), decrypted back by the executor).
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
// 928-955).
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x77}));
}

// Scripts erase_memory's single write + single successful read (legacy lines
// 1279-1345).
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(response({0x71, 0x01, 0x02}));
}

// Scripts reflash_block's "Setting flash start & length..." exchange (legacy
// lines 1092-1127).
void scriptReflashSetup(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x74}));
}

// Scripts the 0xB6 write-chunk sweep for the whole ROM (legacy reflash_block,
// lines 1130-1176). `rom` is encrypted once, matching production.
void scriptReflashChunks(ScriptedCanFlashTransport& transport, bytes::ByteView rom, std::uint32_t chunkSize)
{
    const bytes::Bytes encrypted = toWire(rom);
    for (std::uint32_t addr = 0; addr < rom.size(); addr += chunkSize)
    {
        bytes::Bytes req =
            bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr), bytes::ByteView(encrypted).subspan(addr, chunkSize));
        transport.expectWrite(request(req));
        transport.queueRead(response({0xF6}));
    }
}

// Scripts one close-block attempt (0x37) with the given tail. A tail of
// {0x77} succeeds; anything else is the tolerant-retry loop's "not yet".
void scriptCloseAttempt(ScriptedCanFlashTransport& transport, std::initializer_list<bytes::Byte> tail)
{
    transport.expectWrite(request({0x37}));
    transport.queueRead(response(tail));
}

// Scripts the checksum-verify exchange (legacy lines 1214-1269): UdsClient
// absorbs the intermediate 0x78 (responsePending) NRC by re-reading, so only
// one write is expected even though two reads are queued.
void scriptChecksumVerify(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x01, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x7F, 0x31, 0x78}));
    transport.queueRead(response({0x71, 0x01, 0x02}));
}

bytes::Bytes writeRom()
{
    bytes::Bytes rom(0x80000, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>(i & 0xff);
    }
    return rom;
}

TEST(SubaruHitachiM32rCanExecutor, TransportSetupReturnsThePlansWireParameters)
{
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    const auto setup = executor.transport_setup(plan);

    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e0u);
    EXPECT_EQ(setup->response_id, 0x7e8u);
    EXPECT_FALSE(setup->extended_id);
}

TEST(SubaruHitachiM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    // A plan built for another family must be rejected by check_family before
    // configure()/open() or any write --
    // the scripted transport is left completely untouched.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;

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
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
}

TEST(SubaruHitachiM32rCanExecutor, ConnectAndReadReturnsTheFullRomFromAddressZero)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptBenchConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0, 0x80000, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), 0x80000u);
    EXPECT_TRUE(std::ranges::all_of(*result->read_bytes, [](bytes::Byte b) { return b == 0x5A; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    // The OBK probe and the four identity queries never halt
    // connect_bootloader on their own (legacy lines 91-257 have no early
    // return for a mismatch or an empty reply), so those five exchanges are
    // scripted with ordinary replies. The first exchange with a genuine
    // early-return path on an empty reply is the session-scope probe (legacy
    // line 759's fallthrough `return STATUS_ERROR;`); that is what is
    // scripted here as a dropped frame.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptPreliminaryProbes(transport);
    transport.expectWrite(request({0xA8, 0x00, 0x00, 0x00, 0xD7}));
    transport.queue_no_frame();

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, ReadPropagatesADisconnectedTransport)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptPreliminaryProbes(transport);
    transport.expectWrite(request({0xA8, 0x00, 0x00, 0x00, 0xD7}));
    transport.queueRead(response({0x00, 0xA0, 0x20}));
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));
    transport.expectWrite(request({0x27, 0x01}));
    transport.queueRead(response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));

    bytes::Bytes keyRequest{0x27, 0x02};
    const bytes::Bytes key = seedKey(kSeed);
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(request(keyRequest));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, ConnectRejectsOnCarProgrammingAsUnsupported)
{
    // Session-scope probe response with neither at[1]==0xA0 nor at[2]==0x20
    // selects the on-car branch (legacy lines 281-579), which this port
    // deliberately does not implement -- see the design's on-car scope
    // decision and docs/flash-qualification-matrix.md's
    // FlashEcuSubaruHitachiM32rCan row.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptPreliminaryProbes(transport);
    transport.expectWrite(request({0xA8, 0x00, 0x00, 0x00, 0xD7}));
    transport.queueRead(response({0x00, 0x00, 0x00}));

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
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
    cancellation.cancel();

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

// Cancels the token as soon as the first dump chunk's progress is
// reported, mirroring mitsu_colt_m32r_can_executor_test.cpp's
// CancelAfterFirstChunkSink pattern.
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

TEST(SubaruHitachiM32rCanExecutor, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    CancelAfterFirstChunkSink events{cancellation};
    SubaruHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptBenchConnect(transport);
    scriptDumpSetup(transport);
    // Exactly one dump chunk is scripted; the executor is cancelled while it
    // is being served, so the loop must stop at the top of the next chunk.
    scriptFlashDump(transport, 0, 0x100, 0x100, 0x00);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    const fastecu::RecordedPhaseProgress *last = nullptr;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.phase_name == "Read ROM")
        {
            last = &event;
        }
    }
    ASSERT_NE(last, nullptr);
    EXPECT_LT(last->done, last->total);
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

    scriptBenchConnect(transport);
    // Legacy write_mem (lines 1005-1012) calls erase_memory() before the
    // single reflash_block() call -- the plan and design doc's task-1 brief
    // omits this step, but both the actual legacy source and the design
    // spec's "Portable contract" section (`0x31` RoutineControl erase
    // `0x02 0x01`) confirm it happens; ported faithfully here.
    scriptEraseMemory(transport);
    scriptReflashSetup(transport);
    scriptReflashChunks(transport, rom, 256);
    scriptCloseAttempt(transport, {0x77});
    scriptChecksumVerify(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
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

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("0x80000"));
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruHitachiM32rCanExecutor, WriteToleratesUpToFiveFailedCloseAttemptsBeforeSucceeding)
{
    // Legacy reflash_block's close-block loop retries up to 6 times and
    // proceeds to checksum verification even if every attempt reports
    // something other than 0x77 (the loop's `connected` flag is read nowhere
    // after the loop, lines 1188-1210). Scripts 5 non-0x77 responses followed
    // by a 6th 0x77, and asserts overall success -- pinning the
    // retry-tolerant quirk explicitly.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

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

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
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

    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    scriptReflashSetup(transport);

    const bytes::Bytes encrypted = toWire(rom);
    bytes::Bytes firstChunkRequest =
        bytes::composeBe(bytes::Byte(0xB6), bytes::u24(0), bytes::ByteView(encrypted).subspan(0, 256));
    transport.expectWrite(request(firstChunkRequest));
    transport.queueRead(response({0x7F, 0xB6, 0x22}));

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rCanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    // Unlike Colt, this family's own validate_subaru_hitachi_m32r_can_plan
    // rejects TestWrite outright (Step 5's plan code), so execute() refuses
    // before configure()/open() or any exchange at all -- there is no
    // connect handshake to script here.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto plan = handBuiltPlan(FlashOperation::TestWrite, 0x80000);

    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
}

} // namespace
