#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_types.h"
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
#include <optional>
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
#include "src/backend/flash/ecu/testing/can_executor_run.h"
#include "src/backend/flash/testing/faulting_can_flash_transport.h"
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
using fastecu::flash::BuildSubaruTcuCvtMitsuMh8111CanPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanExecutor;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using ::testing::_;
using ::testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using ::testing::Optional;

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8111_can";
constexpr std::string_view kMcu = "MH8111";
constexpr std::uint32_t kReadStart = 0x8000;
constexpr std::uint32_t kReadLength = 0x78000;
constexpr std::uint32_t kWriteStart = 0x80000;
constexpr std::uint32_t kWriteLength = 0x100000;
constexpr std::uint32_t kImageSize = 0x180000;

// This family's own request/reply envelope -- every exchange (unlike Task
// 3's Hitachi CAN sibling) is sent on 0x7e1/0x7e9; there is no second id.
bytes::Bytes Request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7e1);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes Request(std::initializer_list<bytes::Byte> payload)
{
    return Request(bytes::ByteView(payload.begin(), payload.size()));
}
bytes::Bytes Response(bytes::ByteView tail)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7e9);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}
bytes::Bytes Response(std::initializer_list<bytes::Byte> tail)
{
    return Response(bytes::ByteView(tail.begin(), tail.size()));
}

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
{
    return BuildSubaruTcuCvtMitsuMh8111CanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruTcuCvtMitsuMh8111CanPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_tcu_cvt_mitsu_mh8111_can_plan,
// so a plan whose image size or operation the builder itself would refuse
// can still reach the executor -- proving the executor's own
// validate_subaru_tcu_cvt_mitsu_mh8111_can_plan call rejects it before any
// I/O, not just the builder.
fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation, std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kWriteStart, kWriteLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kWriteStart, kWriteLength}};
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = SubaruTcuCvtMitsuMh8111CanPlan{0x7e1, 0x7e9, 500000, false};
    return fastecu::flash::ValidateAndBuild(std::move(fields));
}

// Payload constants transcribed independently from legacy encrypt_payload,
// not read back from the executor's own translation unit.
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};

// calculatePayload's Feistel structure is memoryless per 4-byte word
// (position-independent), so this single helper both (a) pre-encrypts a
// known plaintext into the wire bytes a scripted read reply must carry for
// the executor's decrypt step to recover it, and (b) computes the wire
// bytes a write must carry for a known plaintext image.
bytes::Bytes ToWire(bytes::ByteView plain)
{
    return ssm_protocol::CalculatePayload(plain, kEncryptTable, ssm_protocol::kStockPairTable);
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// TCU ID (0xAA) / CAL ID (0x09/0x04): both non-fatal, scripted with a valid-
// if-uninteresting reply.
void ScriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("identity queries");
    transport.Exchange(Request({0xAA}), Response({0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    transport.Exchange(Request({0x09, 0x04}), Response({0x49, 0x04, 'C', 'A', 'L'}));
}

// Session 0x10/0x43, non-fatal.
void ScriptSession(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("session");
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
}

// Seed (0x27/0x01) and seed key (0x27/0x02), both fatal.
void ScriptSeedAndKey(ScriptedCanFlashTransport& transport, bytes::ByteView seed, bytes::ByteView key)
{
    const auto section = transport.Section("seed and key");
    bytes::Bytes seed_response{0x67, 0x01};
    seed_response.insert(seed_response.end(), seed.begin(), seed.end());
    transport.Exchange(Request({0x27, 0x01}), Response(seed_response));

    bytes::Bytes key_request{0x27, 0x02};
    key_request.insert(key_request.end(), key.begin(), key.end());
    transport.Exchange(Request(key_request), Response({0x67, 0x02}));
}

// Jump 0x10/0x42, fatal.
void ScriptJump(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("jump");
    transport.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42}));
}

// Alive check: sent 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/ 0x00, checked
// against a 0x71-prefixed reply -- a different service's SID+0x40, not this
// PDU's own (0x74). Confirmed directly against the legacy source (re-read
// twice); see the file header comment.
void ScriptAliveCheck(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("alive check");
    transport.Exchange(Request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       Response({0x71, 0x02, 0x02, 0x03}));
}

// Scripts the full 7-exchange connect_bootloader sequence -- there is no
// kernel-alive pre-check shortcut for this family (unlike Task 3's Hitachi
// CAN sibling), so this always runs in full.
void ScriptFullConnect(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("full connect");
    ScriptIdentityQueries(transport);
    ScriptSession(transport);
    const bytes::Bytes key{0x12, 0xb6, 0xce, 0xba};
    ScriptSeedAndKey(transport, kSeed, key);
    ScriptJump(transport);
    ScriptAliveCheck(transport);
}

// Scripts the "Setting dump start & length..." exchange (legacy read_mem):
// sent 0x35-prefixed, checked against a 0x74-prefixed reply -- again a service
// mismatch, confirmed directly against source.
void ScriptDumpSetup(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("dump setup");
    transport.Exchange(Request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
                       Response({0x74, 0x20, 0x01, 0x04}));
}

// Scripts the chunked 0xB7 dump sweep over [start, start+length) at
// `pagesize`-byte pages, each page filled with `fill` (plaintext -- the
// scripted wire bytes are toWire(fill-page), decrypted back by the
// executor).
void ScriptFlashDump(ScriptedCanFlashTransport& transport, std::uint32_t start, std::uint32_t length,
                     std::uint32_t pagesize, bytes::Byte fill)
{
    const auto section = transport.Section("flash dump");
    const bytes::Bytes plain_page(pagesize, fill);
    const bytes::Bytes wire_page = ToWire(plain_page);
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        bytes::Bytes reply = Response({0xF7});
        reply.insert(reply.end(), wire_page.begin(), wire_page.end());
        transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), bytes::U24{addr})), reply);
    }
}

// Scripts the "Sending stop command..." exchange (legacy read_mem): content-
// blind, succeeding on the first non-empty reply.
void ScriptStopCommand(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("stop command");
    transport.Exchange(Request({0x37}), Response({0x77}));
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
// directly and classifies an absent reply as ErrorKind::kBadResponse ("dump
// start & length setup rejected"), not Timeout -- a different assertion,
// not just a different constant, from what the shared
// ReadReportsAnEmptyReplyAsTimeout test checks. The seed request, by
// contrast, goes through the standard FatalQuery/UdsClient::request path,
// which maps every one of these three failure modes exactly the way the
// conformance suite expects, so it is the cut point used here instead.
void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& transport)
{
    ScriptIdentityQueries(transport);
    ScriptSession(transport);
    const auto section = transport.Section("seed (first request only)");
    transport.Exchange(Request({0x27, 0x01}));
}

// Scripts erase_mem's single successful attempt (-- see the file header
// comment on the evident retry-until-match intent this ports instead of the
// literal always-failing loop).
void ScriptEraseMemory(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("erase memory");
    transport.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}), Response({0x71, 0x01, 0x02}));
}

enum class ChunkReply
{
    kPositive,
    kSilent,
    kUnexpected,
    kDisconnected,
};

// Scripts unlock_and_reflash_block's setup, 256-byte chunk sweep (content-
// blind), close and checksum, each succeeding on the first attempt.
void ScriptWriteBlock(ScriptedCanFlashTransport& transport, bytes::ByteView block_plain,
                      ChunkReply chunk_reply = ChunkReply::kPositive, int close_failures = 0)
{
    const auto section = transport.Section("write block");
    constexpr std::uint32_t kChunkSize = 256;
    // Legacy's own halved data_len bug (maxblocks*128 instead of *256, see
    // reflash_block's comment): kWriteLength/256 * 128 == kWriteLength/2.
    constexpr std::uint32_t kSetupDataLen = (kWriteLength / kChunkSize) * 128;

    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0x34), bytes::Byte(0x04), bytes::Byte(0x33), bytes::U24{0},
                                                bytes::U24{kSetupDataLen})),
                       Response({0x74}));

    ASSERT_THAT(block_plain.first(16),
                testing::ElementsAreArray(bytes::Bytes{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                                       0x0b, 0x0c, 0x0d, 0x0e, 0x0f}));
    bytes::Bytes encrypted = ToWire(block_plain);
    // Independently calculated first wire word sequence; do not derive from ToWire.
    constexpr std::array<bytes::Byte, 16> kFirstWire{0x76, 0xf9, 0xec, 0x8b, 0x7d, 0x53, 0x6f, 0xbf,
                                                     0x50, 0xe5, 0x3f, 0x61, 0x16, 0xc7, 0xd5, 0x5c};
    std::copy(kFirstWire.begin(), kFirstWire.end(), encrypted.begin());
    for (std::uint32_t offset = 0; offset < kWriteLength; offset += kChunkSize)
    {
        const std::uint32_t addr = kWriteStart + offset;
        bytes::Bytes req = bytes::ComposeBe(bytes::Byte(0xB6), bytes::U24{addr},
                                            bytes::ByteView(encrypted).subspan(offset, kChunkSize));
        transport.Exchange(Request(req));
        if (chunk_reply == ChunkReply::kSilent)
        {
            transport.QueueNoFrame();
        }
        else if (chunk_reply == ChunkReply::kUnexpected)
        {
            transport.QueueRead(Response({0x7F, 0xB6, 0x22}));
        }
        else if (chunk_reply == ChunkReply::kDisconnected)
        {
            transport.QueueError(ErrorKind::kDisconnected, "chunk adapter gone");
            return;
        }
        else
        {
            transport.QueueRead(Response({0xF6}));
        }
    }

    for (int attempt = 0; attempt < close_failures; ++attempt)
    {
        transport.Exchange(Request({0x37}));
        transport.QueueNoFrame();
    }
    if (close_failures == 20)
    {
        return;
    }
    transport.Exchange(Request({0x37}), Response({0x77}));

    transport.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}), Response({0x71, 0x01, 0x02}));
}

bytes::Bytes WriteRom()
{
    bytes::Bytes rom(kImageSize, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>(i);
    }
    return rom;
}

// Literal vectors calculated with a standalone 16-bit Feistel transcription.
// They pin the local table binding and exact security arbitration id, not a
// hardware-qualified seed/key oracle or interchangeable connection policy.
TEST(SubaruTcuCvtMitsuMh8111CanExecutor, SecurityUsesLiteralStockVectorsOnFamilyAddress)
{
    const std::array<std::pair<bytes::Bytes, bytes::Bytes>, 2> vectors{{
        {{0x00, 0x00, 0x00, 0x00}, {0xc2, 0x61, 0x58, 0x3a}},
        {{0xff, 0xff, 0xff, 0xff}, {0xe3, 0xa0, 0x6c, 0x00}},
    }};
    for (const auto& [seed, key] : vectors)
    {
        SCOPED_TRACE(testing::PrintToString(seed));
        ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
        FakeClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruTcuCvtMitsuMh8111CanExecutor executor;
        auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ScriptIdentityQueries(transport);
        ScriptSession(transport);
        ScriptSeedAndKey(transport, seed, key);
        // Stop immediately after the exact seed-key exchange has succeeded.
        transport.Exchange(Request({0x10, 0x42}));
        transport.QueueError(ErrorKind::kDisconnected, "stop after security");

        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kDisconnected));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteAcceptsSilentChunkReplies)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptEraseMemory(transport);
    ScriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength), ChunkReply::kSilent);

    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteAcceptsUninspectedChunkReplies)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptEraseMemory(transport);
    ScriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength), ChunkReply::kUnexpected);

    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WritePropagatesChunkTransportError)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptEraseMemory(transport);
    ScriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength), ChunkReply::kDisconnected);

    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteCloseExhaustionIsFatal)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptEraseMemory(transport);
    ScriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength), ChunkReply::kPositive, 20);

    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("block close failed")));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteCloseCanSucceedOnTwentiethAttempt)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptEraseMemory(transport);
    ScriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength), ChunkReply::kPositive, 19);

    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ConnectFullSequenceEveryTime)
{
    // Proves there is no alive-skip shortcut (unlike Task 3): scripts every
    // one of the 7 connect_bootloader exchanges in exact order. The fake
    // does byte-exact matching, so a skipped or reordered exchange would
    // fail the very next write comparison instead of silently passing.
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptDumpSetup(transport);
    ScriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    ScriptStopCommand(transport);

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadReturnsTheLowerWindowPaddedWithFF)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptDumpSetup(transport);
    ScriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    ScriptStopCommand(transport);

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    ASSERT_EQ(result->read_bytes->size(), kReadStart + kReadLength);
    // Padded with 0xFF, NOT 0x00 -- this family differs from Task 3's
    // Hitachi CAN sibling here.
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(kReadStart), test_bytes::BytesFilledWith(0xFF));
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).subspan(kReadStart), test_bytes::BytesFilledWith(0x5A));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    cancellation.Cancel();

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_mem's 0x35 dump-setup exchange -- routed through
// ctx.channel directly but still a single, non-looped call -- this pins a
// transport error raised *inside* the 0xB7 dump-chunk loop, whose reads go
// through FatalRequest at a different call site, inside a `for` loop
// carrying its own cancellation-check and progress-accumulation state.
// FatalRequest's generic error propagation is covered once for every family
// by uds_client_exchange_common_test.cpp; this test is what actually proves
// the loop itself aborts cleanly -- without corrupting rom/progress state --
// on a transport error, rather than assuming FatalRequest's coverage
// implies the loop wrapping it behaves the same way.
TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptDumpSetup(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), bytes::U24{kReadStart})));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteErasesThenFlashesTheUpperBlockAtAddressAbove0x80000)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptFullConnect(transport);
    ScriptEraseMemory(transport);
    ScriptWriteBlock(transport, bytes::ByteView(rom).subspan(kWriteStart, kWriteLength));

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruTcuCvtMitsuMh8111CanExecutor executor;

    // build_subaru_tcu_cvt_mitsu_mh8111_can_plan rejects this image, but
    // validate_and_build does not. The executor must still reject it before
    // it reaches the TCU handshake.
    auto plan = HandBuiltPlan(FlashOperation::kWrite, kImageSize - 1);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("0x180000")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_THAT(events.logs, IsEmpty());
}

template <typename Transport>
fastecu::Result<fastecu::flash::FlashExecutionResult> RunRead(Transport& transport, RecordingEventSink& events)
{
    const auto plan = ReadPlan();
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    return SubaruTcuCvtMitsuMh8111CanExecutor{}.Execute(*plan, transport, clock, cancellation, events);
}

bool ContainsLog(const RecordingEventSink& events, std::string_view substring)
{
    return std::ranges::any_of(events.logs,
                               [&](const auto& entry) { return entry.second.find(substring) != std::string::npos; });
}

// The full connect is seven exchanges, so the dump setup is the eighth.
constexpr std::size_t kDumpSetupExchange = 8;

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ADumpSetupTransportFaultPropagates)
{
    for (const bool on_read : {false, true})
    {
        SCOPED_TRACE(::testing::PrintToString(on_read));
        fastecu::flash::FaultingCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
        (on_read ? transport.fail_read_at : transport.fail_write_at) = kDumpSetupExchange;
        ScriptFullConnect(transport);
        ScriptDumpSetup(transport);
        RecordingEventSink events;
        EXPECT_THAT(RunRead(transport, events), fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("injected")));
        EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>(kDumpSetupExchange - (on_read ? 0 : 1)));
        EXPECT_EQ(transport.WritesConsumed(), *transport.writes_at_fault);
    }
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, ASilentWrongOrShortDumpSetupReplyIsRejected)
{
    const bytes::Bytes request = Request({0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00});
    for (const auto& [reply, log] : std::to_array<std::pair<std::optional<bytes::Bytes>, std::string_view>>({
             {std::nullopt, "No valid response from ECU"},
             {Response({0x74, 0x20, 0x01, 0x05}), "Wrong response from TCU: "},
             {Response({0x74, 0x20, 0x01}), "Wrong response from TCU: "},
         }))
    {
        SCOPED_TRACE(log);
        ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
        ScriptFullConnect(transport);
        if (reply.has_value())
        {
            transport.Exchange(request, *reply);
        }
        else
        {
            transport.Exchange(request);
            transport.QueueNoFrame();
        }
        RecordingEventSink events;
        EXPECT_THAT(RunRead(transport, events),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq("dump start & length setup rejected")));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_TRUE(ContainsLog(events, log));
    }
}

TEST(SubaruTcuCvtMitsuMh8111CanExecutor, AFailedStopSendFailsTheRead)
{
    fastecu::flash::FaultingCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.fail_write_at = kDumpSetupExchange + kReadLength / 0x100 + 1;
    ScriptFullConnect(transport);
    ScriptDumpSetup(transport);
    ScriptFlashDump(transport, kReadStart, kReadLength, 0x100, 0x5A);
    RecordingEventSink events;
    EXPECT_THAT(RunRead(transport, events), fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("injected")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(ContainsLog(events, "ROM read complete"));
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct TcuCvtMitsuMh8111CanTraits
    : fastecu::flash::testing::CanScriptForwarders<
          &::ReadPlan, [](FlashOperation op) { return ::HandBuiltPlan(op, kImageSize); }, &::ScriptFullConnect,
          &::ScriptDumpSetup, &::ScriptFlashDump, &::ScriptStopCommand, &::ScriptUpToFirstFatalRead>
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
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruTcuCvtMitsuMh8111Can, CanExecutorConformance, TcuCvtMitsuMh8111CanTraits);

} // namespace
