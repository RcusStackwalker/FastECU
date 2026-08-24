// Equivalence tests for SubaruTcuCvtMitsuMh8111CanExecutor, the portable
// replacement for FlashTcuCvtSubaruMitsuMH8111CanOperation's
// connect_bootloader(), read_mem(), write_mem(), reflash_block() and
// erase_mem(). Expected wire bytes are transcribed character-for-character
// from
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp,
// with two disclosed deliberate divergences from the literal source: (1)
// erase_mem's own retry loop never resends and never breaks on success in
// legacy (so write_mem always failed at the erase step in production) --
// this ports the evident retry-until-match intent shared by every sibling
// retry loop in the same file instead; (2) the "alive check" exchange in
// connect_bootloader sends a 0x34-prefixed PDU but checks for a
// 0x71-prefixed reply (a different service's SID+0x40), which the brief's
// own Step 7 description got wrong (it describes Task 3's alive-recheck
// bytes instead) -- the executor uses the bytes actually found in the
// legacy source.
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
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_tcu_cvt_mitsu_mh8111_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanExecutor;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanPlan;
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

fastecu::flash::FlashPlan readPlan()
{
    auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_tcu_cvt_mitsu_mh8111_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_mitsu_mh8111_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation, std::size_t image_size)
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
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt/decrypt tables, transcribed independently from the same
// legacy lines the executor was (generate_seed_key/encrypt_payload/
// decrypt_payload, lines 904-977) -- not read back from the executor's own
// translation unit.
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

// TCU ID (0xAA, legacy lines 94-133) / CAL ID (0x09/0x04, lines 135-168):
// both non-fatal, scripted with a valid-if-uninteresting reply.
void scriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0xAA}));
    transport.queueRead(response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.expectWrite(request({0x09, 0x04}));
    transport.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session 0x10/0x43 (lines 170-197), non-fatal.
void scriptSession(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));
}

// Seed (0x27/0x01) and seed key (0x27/0x02), both fatal (lines 199-263).
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

// Jump 0x10/0x42, fatal (lines 267-295).
void scriptJump(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x10, 0x42}));
    transport.queueRead(response({0x50, 0x42}));
}

// Alive check (lines 298-331): sent 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/
// 0x00, checked against a 0x71-prefixed reply -- a different service's
// SID+0x40, not this PDU's own (0x74). Confirmed directly against the
// legacy source (re-read twice); see the file header comment.
void scriptAliveCheck(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x71, 0x02, 0x02, 0x03}));
}

// Scripts the full 7-exchange connect_bootloader sequence -- there is no
// kernel-alive pre-check shortcut for this family (unlike Task 3's Hitachi
// CAN sibling), so this always runs in full.
void scriptFullConnect(ScriptedCanFlashTransport& transport)
{
    scriptIdentityQueries(transport);
    scriptSession(transport);
    const bytes::Bytes key = seedKey(kSeed);
    scriptSeedAndKey(transport, kSeed, key);
    scriptJump(transport);
    scriptAliveCheck(transport);
}

// Scripts the "Settting dump start & length..." exchange (legacy read_mem,
// lines 365-400): sent 0x35-prefixed, checked against a 0x74-prefixed
// reply -- again a service mismatch, confirmed directly against source.
void scriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x74, 0x20, 0x01, 0x04}));
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
// 504-526): content-blind, succeeding on the first non-empty reply.
void scriptStopCommand(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x77}));
}

// Scripts erase_mem's single successful attempt (legacy lines 833-892 --
// see the file header comment on the evident retry-until-match intent this
// ports instead of the literal always-failing loop).
void scriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(response({0x71, 0x01, 0x02}));
}

// Scripts unlock_and_reflash_block's setup, 256-byte chunk sweep
// (content-blind), close and checksum (legacy lines 636-826), each
// succeeding on the first attempt.
void scriptWriteBlock(ScriptedCanFlashTransport& transport, bytes::ByteView blockPlain)
{
    constexpr std::uint32_t kChunkSize = 256;
    // Legacy's own halved data_len bug (maxblocks*128 instead of *256, see
    // reflash_block's comment): kWriteLength/256 * 128 == kWriteLength/2.
    constexpr std::uint32_t kSetupDataLen = (kWriteLength / kChunkSize) * 128;

    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33),
                                                   bytes::u24(0), bytes::u24(kSetupDataLen))));
    transport.queueRead(response({0x74}));

    const bytes::Bytes encrypted = toWire(blockPlain);
    for (std::uint32_t offset = 0; offset < kWriteLength; offset += kChunkSize)
    {
        const std::uint32_t addr = kWriteStart + offset;
        bytes::Bytes req = bytes::composeBe(bytes::Byte(0xB6), bytes::u24(addr),
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        transport.expectWrite(request(req));
        transport.queueRead(response({0xF6}));
    }

    transport.expectWrite(request({0x37}));
    transport.queueRead(response({0x77}));

    transport.expectWrite(request({0x31, 0x01, 0x02, 0x02, 0x01}));
    transport.queueRead(response({0x71, 0x01, 0x02}));
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

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, TransportSetupReturnsThePlansWireParameters)
{
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const auto plan = readPlan();

    const auto setup = executor.transport_setup(plan);

    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e1u);
    EXPECT_EQ(setup->response_id, 0x7e9u);
    EXPECT_FALSE(setup->extended_id);
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;

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

    transport.start_open();
    const auto result = executor.execute(*foreign, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("does not match this executor"));
    EXPECT_THAT(events.logs, IsEmpty());
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ConnectFullSequenceEveryTime)
{
    // Proves there is no alive-skip shortcut (unlike Task 3): scripts every
    // one of the 7 connect_bootloader exchanges in exact order. expectWrite
    // does byte-exact matching, so a skipped or reordered exchange would
    // fail the very next write comparison instead of silently passing.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadReturnsTheLowerWindowPaddedWithFF)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    scriptStopCommand(transport);

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
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

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    // The first exchange with a genuine early-return path on an empty reply
    // is the fatal seed request (legacy lines 199-225); TCU ID/CAL ID/
    // session never halt connect_bootloader on their own.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();

    scriptIdentityQueries(transport);
    scriptSession(transport);
    transport.expectWrite(request({0x27, 0x01}));
    transport.queue_no_frame();

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();
    cancellation.cancel();

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

// Cancels the token as soon as the first dump chunk's progress is
// reported, mirroring Task 1/3's own CancelAfterFirstChunkSink pattern.
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

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    // Exercises the cancellation check at the top of dump_flash_range's
    // page loop (legacy stopRequested(), line 421): connect and the first
    // 0x100 dump chunk are scripted, cancel() lands on that chunk's
    // progress event, and the loop must stop before requesting a second
    // chunk -- there is no second chunk scripted, so any further write
    // would fail against the exhausted script instead.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    CancelAfterFirstChunkSink events{cancellation};
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    scriptFlashDump(transport, kReadStart, 0x100, 0x100, 0x5A);

    transport.start_open();
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

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadPropagatesADisconnectedTransport)
{
    // A transport-level Disconnected failure mid-read must surface as
    // ErrorKind::Disconnected, not be swallowed or misclassified as a
    // malformed/timeout response.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = readPlan();

    scriptFullConnect(transport);
    scriptDumpSetup(transport);
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), bytes::u24(kReadStart))));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteErasesThenFlashesTheUpperBlockAtAddressAbove0x80000)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptFullConnect(transport);
    scriptEraseMemory(transport);
    scriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength));

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;

    // build_subaru_tcu_cvt_mitsu_mh8111_can_plan rejects this image, but
    // validate_and_build does not. The executor must still reject it before
    // it reaches the TCU handshake.
    auto plan = handBuiltPlan(FlashOperation::Write, kImageSize - 1);

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("0x180000"));
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    // cfg test_write=no for this family; build_subaru_tcu_cvt_mitsu_mh8111_can_plan
    // rejects TestWrite outright (Step 5's plan code), so this pins the
    // executor's own repeated guard using a hand-built plan that bypasses
    // the builder -- there is no connect handshake to script here.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = handBuiltPlan(FlashOperation::TestWrite, kImageSize);

    transport.start_open();
    const auto result = executor.execute(plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

} // namespace
