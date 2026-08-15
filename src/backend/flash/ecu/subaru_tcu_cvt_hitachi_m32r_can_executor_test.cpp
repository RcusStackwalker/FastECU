// Equivalence tests for SubaruTcuCvtHitachiM32rCanExecutor, the portable
// replacement for FlashTcuCvtSubaruHitachiM32rCanOperation's
// connect_bootloader(), read_mem(), write_mem(), reflash_block() and
// erase_mem() -- the REAL logic execute() never reached (it called the
// always-failing hack_words() instead). Expected wire bytes are transcribed
// character-for-character from
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp's
// connect_bootloader/read_mem/write_mem/reflash_block/erase_mem.
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
#include "src/backend/flash/flash_cancellation.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

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

fastecu::flash::FlashPlan readPlan()
{
    auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu,
                                                           std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                           std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_tcu_cvt_hitachi_m32r_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_hitachi_m32r_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation, std::size_t image_size)
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
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload, lines 1051-1122) -- not read back from the executor's own
// translation unit. Mirrors subaru_hitachi_m32r_can_executor_test.cpp's own
// precedent.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
    0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes seedKey(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable.data(), kIndexTransformation.data());
}

// calculatePayload's Feistel structure inverts by reversing key order and is
// memoryless per 4-byte word (position-independent), so this single helper
// both (a) pre-encrypts a known plaintext into the wire bytes a scripted
// read reply must carry for the executor's decrypt step to recover it, and
// (b) computes the wire bytes a write must carry for a known plaintext
// image -- even for a sub-window that does not start at image offset 0.
bytes::Bytes toWire(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()),
                                         kEncryptTable.data(), kIndexTransformation.data());
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// Kernel-alive probe (legacy lines 100-129), scripted as a miss (no frame at
// all -- legacy's "No valid response from ECU" branch), which falls through
// to full initialization rather than returning early.
void scriptKernelAliveMiss(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x02, 0x02, 0x01}));
    transport.queue_no_frame();
}

void scriptKernelAliveHit(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x71, 0x02, 0x02, 0x03}));
}

// TCU ID / CAL ID queries (legacy lines 137-211): sent on 0x7E0, non-fatal,
// scripted with a valid-if-uninteresting reply.
void scriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(requestOnId(0x7e0, {0xAA}));
    transport.queueRead(response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.expectWrite(requestOnId(0x7e0, {0x09, 0x04}));
    transport.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session (0x10/0x03 fatal, 0x10/0x43 non-fatal) and seed/seed-key
// exchanges (legacy lines 216-333): all sent on 0x7E0.
void scriptSessionAndSeed(ScriptedCanFlashTransport& transport, bytes::ByteView seed,
                          bytes::ByteView key)
{
    transport.expectWrite(requestOnId(0x7e0, {0x10, 0x03}));
    transport.queueRead(response({0x50, 0x03}));

    transport.expectWrite(requestOnId(0x7e0, {0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));

    transport.expectWrite(requestOnId(0x7e0, {0x27, 0x01}));
    bytes::Bytes seedResponse{0x67, 0x01};
    seedResponse.insert(seedResponse.end(), seed.begin(), seed.end());
    transport.queueRead(response(seedResponse));

    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(requestOnId(0x7e0, keyRequest));
    transport.queueRead(response({0x67, 0x02}));
}

// Jump (0x10/0x02) and alive re-check (0x31/0x02/0x02/0x01), both back on
// this family's own 0x7e1 (legacy lines 339-401).
void scriptJumpAndRecheck(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x10, 0x02}));
    transport.queueRead(response({0x50, 0x02}));

    transport.expectWrite(request({0x31, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x71, 0x02, 0x02, 0x03}));
}

// Scripts the full connect_bootloader sequence with the kernel-alive probe
// missing (the "kernel not running" branch).
void scriptFullConnect(ScriptedCanFlashTransport& transport)
{
    scriptKernelAliveMiss(transport);
    scriptIdentityQueries(transport);
    const bytes::Bytes key = seedKey(kSeed);
    scriptSessionAndSeed(transport, kSeed, key);
    scriptJumpAndRecheck(transport);
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem,
// lines 529-562) over the resolved {0x8000, 0x78000} window.
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04),
                                                   bytes::Byte(0x33), bytes::u24(0x8000),
                                                   bytes::u24(0x78000))));
    transport.queueRead(response({0x74, 0x20, 0x01, 0x04}));
}

// Scripts the chunked 0xB7 dump sweep over [start, start+length) at
// `pagesize`-byte pages, each page filled with `fill` (plaintext -- the
// scripted wire bytes are toWire(fill-page), decrypted back by the
// executor).
void scriptFlashDump(ScriptedCanFlashTransport& transport, std::uint32_t start,
                     std::uint32_t length, std::uint32_t pagesize, bytes::Byte fill)
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
// 673-700). A well-formed 0x77 reply is success through fatal_request()'s
// standard SID+0x40 matching -- see the executor's comment on legacy's own
// inverted polarity there.
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x77}));
}

// Scripts erase_mem's single write + single successful read (legacy lines
// 991-1039). The positive response echoes the request SID literally (0x31),
// not the standard SID+0x40 convention.
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x02, 0x01, 0xff, 0xff, 0xff, 0xff}));
    transport.queueRead(response({0x31, 0x02, 0x01}));
}

// Scripts one reflash_block block: setup, 128-byte chunk sweep
// (content-blind -- any well-formed reply is accepted), close, checksum
// (legacy lines 811-984).
void scriptWriteBlock(ScriptedCanFlashTransport& transport, bytes::ByteView blockPlain,
                      std::uint32_t start, std::uint32_t length)
{
    constexpr std::uint32_t kChunkSize = 128;

    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04),
                                                   bytes::Byte(0x33), bytes::u24(start),
                                                   bytes::u24(length))));
    transport.queueRead(response({0x74}));

    const bytes::Bytes encrypted = toWire(blockPlain);
    for (std::uint32_t offset = 0; offset < length; offset += kChunkSize)
    {
        const std::uint32_t addr = start + offset;
        bytes::Bytes req = bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr),
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        transport.expectWrite(request(req));
        transport.queueRead(response({0xF6}));
    }

    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x77}));

    transport.expectWrite(request({0x31, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x71, 0x02, 0x02}));
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

TEST(SubaruTcuCvtHitachiM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;

    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = fastecu::flash::FlashFamily::DensoSh705xEepromCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = fastecu::flash::MemoryRegion{.start = 0x0, .length = 0x100};
    fields.kernel = fastecu::flash::KernelImage{
        .id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}};
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

    const auto result =
        executor.execute(*foreign, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("does not match this executor"));
    EXPECT_THAT(events.logs, IsEmpty());
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ConnectSkipsTheRestWhenKernelAlreadyRunning)
{
    // legacy lines 117-129: a matching alive-probe reply returns
    // STATUS_SUCCESS immediately, with zero further writes.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptKernelAliveHit(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0x8000, 0x78000, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    // Only the scripted sequence above was consumed -- if the executor had
    // continued into the rest of connect_bootloader after a matching probe
    // (identity queries, session, seed, jump...), the next write would not
    // match any of the entries scripted here and execute() would have
    // failed above instead.
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ConnectFullSequenceWhenKernelNotRunning)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0x8000, 0x78000, 0x100, 0x00);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ReadReturnsTheFloorClampedWindowPaddedWithZero)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, 0x8000, 0x78000, 0x100, 0x5A);
    scriptStopCommand(transport);

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), 0x80000u);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->begin() + 0x8000,
                            [](bytes::Byte b)
                            { return b == 0x00; }));
    EXPECT_TRUE(std::all_of(result->read_bytes->begin() + 0x8000, result->read_bytes->end(),
                            [](bytes::Byte b)
                            { return b == 0x5A; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    // The first exchange with a genuine early-return path on an empty reply
    // is the fatal session-mode request (legacy lines 216-242); the alive
    // probe and identity queries never halt connect_bootloader on their
    // own.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();

    scriptKernelAliveMiss(transport);
    scriptIdentityQueries(transport);
    transport.expectWrite(requestOnId(0x7e0, {0x10, 0x03}));
    transport.queue_no_frame();

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = readPlan();
    cancellation.trip();

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, WriteErasesThenFlashesEightBlocksOfSixtyFourKib)
{
    // The 8 flashed blocks (M32R_512KB indices 3-10) are NOT uniformly 64
    // KiB: block index 3 is 32 KiB, the remaining seven are 64 KiB each
    // (fblocks_M32R_512KB in kernelmemorymodels.h; the wave-3 plan's Global
    // Constraints table states 0x10000 for block 3, which does not match
    // the source -- see subaru_tcu_cvt_hitachi_m32r_can_executor.cpp's
    // kWriteBlocks comment). Scripted here with the real per-block sizes.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

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

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;

    // build_subaru_tcu_cvt_hitachi_m32r_can_plan rejects this image, but
    // validate_and_build does not. The executor must still reject it before
    // it configures or opens the transport, let alone reaches the TCU
    // handshake.
    auto plan = handBuiltPlan(FlashOperation::Write, 0x7ffff);

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("0x80000"));
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruTcuCvtHitachiM32rCanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    // cfg test_write=no for this family; build_subaru_tcu_cvt_hitachi_m32r_can_plan
    // rejects TestWrite outright (Step 5's plan code), so this pins the
    // executor's own repeated guard using a hand-built plan that bypasses
    // the builder -- there is no connect handshake to script here.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruTcuCvtHitachiM32rCanExecutor executor;
    auto plan = handBuiltPlan(FlashOperation::TestWrite, 0x80000);

    const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
}

} // namespace
