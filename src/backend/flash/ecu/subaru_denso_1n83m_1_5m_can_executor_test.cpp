#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for SubaruDenso1n83m_1_5mCanExecutor, the portable
// replacement for flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp.
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::flash::BuildSubaruDenso1n83m15mCanPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDenso1n83m_1_5mCanExecutor;
using fastecu::flash::SubaruDenso1n83m_1_5mCanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::Each;
using testing::IsEmpty;

// Records every ctx.clock.sleep() argument so the executor's inter-exchange
// settles can be asserted as a sequence. Same shape as the recording clocks in
// subaru_denso_sh7055_02_executor_test.cpp and
// subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp: a
// FakeClock with one extra hook, so no fake or port changes shape.

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_1_5m_can";
constexpr std::string_view kMcu = "N83M_1_5MB";

// kFlashBlocksN83M_1_5MB: [0] = {0x08F9C000, 0x10000}, [1] = {0x08FAC000,
// 0x173F00}, [2] = {0x0911FF00, 0x100}.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kBlockStart = 0x08FAC000;
constexpr std::uint32_t kBlockLength = 0x173F00;
constexpr std::size_t kImageSize = 0x184000;
constexpr std::uint32_t kPageSize = 0x100;

// Every primary request carries the 4-byte big-endian 0x7E0 envelope; every
// primary response the 0x7E8 reply id (legacy connect_bootloader).
bytes::Bytes RequestTo(std::uint32_t id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes RequestTo(std::uint32_t id, std::initializer_list<bytes::Byte> payload)
{
    return RequestTo(id, bytes::ByteView(payload.begin(), payload.size()));
}
bytes::Bytes Request(bytes::ByteView payload)
{
    return RequestTo(0x7e0, payload);
}
bytes::Bytes Request(std::initializer_list<bytes::Byte> payload)
{
    return RequestTo(0x7e0, payload);
}
bytes::Bytes ResponseFrom(std::uint32_t id, std::initializer_list<bytes::Byte> tail)
{
    return RequestTo(id, tail);
}
bytes::Bytes Response(std::initializer_list<bytes::Byte> tail)
{
    return RequestTo(0x7e8, tail);
}

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
{
    return BuildSubaruDenso1n83m15mCanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruDenso1n83m15mCanPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_denso_1n83m_1_5m_can_plan,
// so a plan whose operation the builder itself would refuse can still reach
// the executor -- the only way to prove the executor's own
// validate_subaru_denso_1n83m_1_5m_can_plan call rejects it before any I/O.
fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::kSubaruDenso1n83m15mCan;
    fields.transport = fastecu::flash::TransportKind::kCanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kBlockStart, kBlockLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}};
    fields.image = bytes::Bytes(kImageSize, 0x00);
    fields.family_plan = SubaruDenso1n83m_1_5mCanPlan{0x7e0, 0x7e8, 500000, false, 0x10000, 0x100};
    return fastecu::flash::ValidateAndBuild(std::move(fields));
}

// The seed/encrypt tables, transcribed independently from the same legacy
// lines the executor was
// (generate_can_seed_key/encrypt_payload/decrypt_payload) rather than read
// back from the executor's own translation unit, so a wrong table entry in the
// executor fails these assertions instead of passing silently.
constexpr std::array<std::uint16_t, 4> kEncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

// The executor's decrypt table is this encrypt table exactly reversed, and
// SsmProtocol::calculatePayload's Feistel structure inverts by reversing key
// order, so this single helper both (a) pre-encrypts a known plaintext into
// the wire bytes a scripted read reply must carry for the executor's decrypt
// step to recover it, and (b) computes the wire bytes a write must carry for
// a known plaintext image.
bytes::Bytes ToWire(bytes::ByteView plain)
{
    return ssm_protocol::CalculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptTable,
                                          kIndexTransformation);
}

// The OBK probe miss, the four non-fatal identity queries, the access-method
// probe and the branch selector. Byte 7 of the raw 0x22 0x10 0x1D reply frame
// -- payload index 3 -- selects the programming branch.
void ScriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branch_byte)
{
    const auto section = t.Section("preliminaries");
    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));                          // OBK probe, miss
    t.Exchange(Request({0xAA}), Response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));           // ECU ID
    t.Exchange(Request({0x09, 0x02}), Response({0x49, 0x02, 'V', 'I', 'N'}));           // VIN
    t.Exchange(Request({0x09, 0x04}), Response({0x49, 0x04, 'C', 'A', 'L'}));           // CAL ID
    t.Exchange(Request({0x09, 0x06}), Response({0x49, 0x06, 0xAA, 0xBB}));              // CVN
    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));                          // access method
    t.Exchange(Request({0x22, 0x10, 0x1D}), Response({0x62, 0x10, 0x1D, branch_byte})); // branch selector
}

// The bench arm.
void ScriptBenchConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("bench connect");
    ScriptPreliminaries(t, 0xFF);
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    t.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory).
void ScriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               Response({0x75, 0x20, 0x01, 0x01}));
}

// The chunked 0xB7 dump sweep (legacy read_memory): 0xB7 plus a 4-byte big-
// endian address, answered with 0xF7 plus one encrypted page.
void ScriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length, std::uint32_t pagesize,
                     bytes::Byte fill)
{
    const auto section = t.Section("flash dump");
    const bytes::Bytes wire_page = ToWire(bytes::Bytes(pagesize, fill));
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        bytes::Bytes reply = Response({0xF7});
        reply.insert(reply.end(), wire_page.begin(), wire_page.end());
        t.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), addr)), reply);
    }
}

// The 0x37 stop command (legacy read_memory).
void ScriptStopCommand(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("stop command");
    t.Exchange(Request({0x37}), Response({0x77}));
}

// Connect plus read_memory's 0x34 dump-setup request, registered as an
// expected write only -- its reply is left for the caller to queue, so the
// same script serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there.
void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    ScriptBenchConnect(t);
    const auto section = t.Section("read setup (first request only)");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}));
}

// The in-car arm. The ten fire-and-forget replies are deliberately given
// arbitration ids other than 0x7E8 wherever the addressed module would answer
// on its own id: legacy reads whichever frame arrives next without checking
// the id, and this pins that the port does not add a check legacy lacks.
void ScriptInCarConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("in-car connect");
    ScriptPreliminaries(t, 0x00);

    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01})); // mismatch logs only

    t.Exchange(RequestTo(0x7A2, {0x10, 0xC0}), ResponseFrom(0x7AA, {0x50, 0xC0}));
    t.Exchange(Request({0x10, 0x63}), Response({0x50, 0x63}));
    t.Exchange(RequestTo(0x7DF, {0x10, 0x03}), Response({0x50, 0x03}));
    t.Exchange(RequestTo(0x7E1, {0x10, 0x63}), ResponseFrom(0x7E9, {0x50, 0x63}));
    t.Exchange(RequestTo(0x7B0, {0x10, 0x03}), ResponseFrom(0x7B8, {0x50, 0x03}));
    t.Exchange(RequestTo(0x7B0, {0x85, 0x02}), ResponseFrom(0x7B8, {0xC5, 0x02}));
    t.Exchange(RequestTo(0x7DF, {0x85, 0x02}), Response({0xC5, 0x02}));
    t.Exchange(RequestTo(0x7B0, {0x85, 0x02}), ResponseFrom(0x7B8, {0xC5, 0x02}));
    t.Exchange(RequestTo(0x7DF, {0x85, 0x02}), Response({0xC5, 0x02}));
    t.Exchange(RequestTo(0x7DF, {0x28, 0x03, 0x01}), Response({0x68, 0x03}));

    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));

    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x63}));                   // fatal on mismatch
    t.Exchange(Request({0x22, 0x10, 0x1D}), Response({0x62, 0x10, 0x1D, 0x00})); // fatal on mismatch
    t.Exchange(Request({0x10, 0x62}), Response({0x50, 0x62}));
}

// erase_memory's setup PDU plus its erase trigger; the trigger's answer is
// consumed by the re-read loop, not by a paired read.
void ScriptEraseMemory(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("erase memory");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block). `rom` is the
// whole 0x184000 plan image, encrypted once, and indexed from kImageStart --
// so chunk 0 carries encrypted[0x10000..0x10100).
void ScriptReflashChunks(ScriptedCanFlashTransport& t, bytes::ByteView rom)
{
    const auto section = t.Section("reflash chunks");
    const bytes::Bytes encrypted = ToWire(rom);
    for (std::uint32_t offset = 0; offset < kBlockLength; offset += 256)
    {
        const std::uint32_t addr = kBlockStart + offset;
        t.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB6), addr,
                                            bytes::ByteView(encrypted).subspan(addr - kImageStart, 256))),
                   Response({0xF6}));
    }
}

// The close-block 0x37 and the checksum verify. UdsClient absorbs the
// intermediate 0x78 responsePending NRC by re-reading, so only one write is
// expected even though two reads are queued.
void ScriptCloseAndChecksum(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("close and checksum");
    t.Exchange(Request({0x37}), Response({0x77}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}), Response({0x7F, 0x31, 0x78}));
    t.QueueRead(Response({0x71, 0x01, 0x02}));
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

TEST(SubaruDenso1n83m_1_5mCanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    ASSERT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));                 // leading pad
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x10000, kBlockLength), Each(0xA5)); // decrypted payload
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));                    // tail pad
    EXPECT_TRUE(transport.ScriptConsumed());
    // The seed and key exchanges both read with the shared two-second timeout.
    ASSERT_GT(transport.ReadTimeouts().size(), 9U);
    EXPECT_EQ(transport.ReadTimeouts()[8], 2000ms);
    EXPECT_EQ(transport.ReadTimeouts()[9], 2000ms);
    EXPECT_THAT(events.logs, testing::Not(IsEmpty()));
}

// Legacy (20892df) names the 4MB sibling in all three of this family's
// operator-facing identity lines -- an upstream copy-paste. Every other
// identity string in this executor says 1.5M, so a flash log could not be
// attributed to the executor that produced it. Corrected here; this is a
// deliberate divergence from the legacy text, not a transcription slip.
TEST(SubaruDenso1n83m_1_5mCanExecutor, OperatorFacingLogLinesNameThe1_5MFamily)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());

    EXPECT_THAT(events.logs,
                testing::Contains(testing::Pair(fastecu::LogLevel::kInfo,
                                                "Connecting to ECU Denso 1N83M 1.5MB CAN bootloader, please wait...")));
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::kInfo,
                                                             "Reading ROM from ECU, Denso 1N83M 1.5MB using CAN")));
    EXPECT_THAT(events.logs, testing::Each(testing::Pair(testing::_, testing::Not(testing::HasSubstr("4MB")))));
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptInCarConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    ASSERT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x10000, kBlockLength), Each(0x5A));
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, WriteErasesThenFlashesBlockOne)
{
    ScriptedCanFlashTransport transport;
    const bytes::Bytes rom = WriteRom();

    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    transport.QueueRead(Response({0x71, 0x01, 0x02}));
    ScriptReflashChunks(transport, rom);
    ScriptCloseAndChecksum(transport);

    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
    // The write path's own identity line, the third of the three legacy named
    // the 4MB sibling in.
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::kInfo,
                                                             "Writing ROM to ECU, Denso 1N83M 1.5MB using CAN")));
    EXPECT_THAT(events.logs, testing::Each(testing::Pair(testing::_, testing::Not(testing::HasSubstr("4MB")))));
    // The image base is already pinned by the scripted exchanges:
    // scriptReflashChunks builds every 0xB6 frame from encrypted.subspan(addr
    // - kImageStart, 256). This adds only that calculatePayload is 4-byte-word
    // independent, so the page-at-a-time comparison above is a valid way to
    // express it.
    const bytes::Bytes encrypted = ToWire(rom);
    EXPECT_THAT(bytes::Bytes(encrypted.begin() + 0x10000, encrypted.begin() + 0x10000 + 256),
                test_bytes::BytesEq(ToWire(bytes::ByteView(rom).subspan(0x10000, 256))));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait, the settle after the erase
    // command, and the settle before the checksum-verify write. Asserted as a
    // whole sequence rather than by Contains so that dropping one -- as this
    // port did with the checksum-verify settle -- fails here instead of
    // passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 500ms, 100ms}));
}

// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_memory's 0x34 setup exchange -- a fatal_query call --
// this pins a transport error raised *inside* the 0xB7 dump-chunk loop, whose
// reads go through fatal_request at a different call site, inside a `for`
// loop carrying its own cancellation-check and progress-accumulation state.
// fatal_request's generic error propagation is covered once for every family
// by uds_client_exchange_common_test.cpp; this test is what actually proves
// the loop itself aborts cleanly -- without corrupting rom/progress state --
// on a transport error, rather than assuming fatal_request's coverage
// implies the loop wrapping it behaves the same way.
TEST(SubaruDenso1n83m_1_5mCanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, NegativeResponseDuringConnectFails)
{
    // The mirror of 1n83m_4m's tolerated checks: this family is strict, so a
    // negative response to the seed request, which returns STATUS_ERROR, must
    // abort rather than be logged and stepped over.
    ScriptedCanFlashTransport transport;
    ScriptPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x7F, 0x27, 0x35}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, NegativeResponseAtDumpSetupFails)
{
    // The mirror image of subaru_denso_1n83m_4m_can_executor_test's
    // ProceedsPast... cases. That family has the `return STATUS_ERROR` at
    // read_memory's 0x34/0x35 setup checks commented out and steps over a bad
    // reply; this one keeps it live, so the very same exchange must abort
    // here. The pair of tests is what stops the difference being normalized
    // away in either direction.
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                       Response({0x7F, 0x34, 0x31}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, EmptyBranchSelectorReplyFails)
{
    // An absent 0x22 0x10 0x1D reply is one of only two points in the
    // preliminary phase that return STATUS_ERROR.
    ScriptedCanFlashTransport transport;
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    transport.Exchange(Request({0xAA}), Response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));
    transport.Exchange(Request({0x09, 0x02}), Response({0x49, 0x02, 'V', 'I', 'N'}));
    transport.Exchange(Request({0x09, 0x04}), Response({0x49, 0x04, 'C', 'A', 'L'}));
    transport.Exchange(Request({0x09, 0x06}), Response({0x49, 0x06, 0xAA, 0xBB}));
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    transport.Exchange(Request({0x22, 0x10, 0x1D}));
    transport.QueueNoFrame();

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, EraseRetryExhaustionFails)
{
    // Legacy erase_memory's re-read loop: twenty reads, no re-send, then
    // "Flash area erase failed".
    ScriptedCanFlashTransport transport;
    const bytes::Bytes rom = WriteRom();
    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.QueueRead(Response({0x71, 0x01, 0x03}));
    }

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    // A second trigger would have hit the end of the script and surfaced as an
    // Internal error rather than BadResponse; the failure is the erase's own.
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::kError, "Flash area erase failed")));
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Denso1n83m_1_5mCanTraits
{
    using Executor = SubaruDenso1n83m_1_5mCanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // connect_bootloader's tolerant_probe exchanges read with this family's
    // short timeout, a deliberate per-family value (subaru_denso_sh72543_can_diesel
    // probes with 2000ms instead).
    static constexpr std::chrono::milliseconds kProbeTimeout{200};
    static constexpr int kProbeCount = 7; // seed and key now read at 2000ms

    static fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
    {
        return ::ReadPlan();
    }

    static fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
    {
        return ::HandBuiltPlan(operation);
    }

    static void ScriptBenchConnect(ScriptedCanFlashTransport& t)
    {
        ::ScriptBenchConnect(t);
    }

    static void ScriptReadSetup(ScriptedCanFlashTransport& t)
    {
        ::ScriptReadSetup(t);
    }

    static void ScriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length,
                                std::uint32_t pagesize, bytes::Byte fill)
    {
        ::ScriptFlashDump(t, start, length, pagesize, fill);
    }

    static void ScriptStopCommand(ScriptedCanFlashTransport& t)
    {
        ::ScriptStopCommand(t);
    }

    static void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
    {
        ::ScriptUpToFirstFatalRead(t);
    }
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDenso1n83m_1_5mCan, CanExecutorConformance, Denso1n83m_1_5mCanTraits);

} // namespace
