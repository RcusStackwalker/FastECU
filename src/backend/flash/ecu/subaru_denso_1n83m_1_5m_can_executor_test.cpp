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
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_denso_1n83m_1_5m_can_plan;
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
// settles can be asserted as a sequence. Same shape as the wave-2
// (subaru_denso_sh7055_02_executor_test.cpp) and wave-3
// (subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp) recording clocks: a
// FakeClock with one extra hook, so no fake or port changes shape.
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

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_1_5m_can";
constexpr std::string_view kMcu = "N83M_1_5MB";

// fblocks_N83M_1_5MB: [0] = {0x08F9C000, 0x10000}, [1] = {0x08FAC000,
// 0x173F00}, [2] = {0x0911FF00, 0x100}.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kBlockStart = 0x08FAC000;
constexpr std::uint32_t kBlockLength = 0x173F00;
constexpr std::size_t kImageSize = 0x184000;
constexpr std::uint32_t kPageSize = 0x100;

// Every primary request carries the 4-byte big-endian 0x7E0 envelope; every
// primary response the 0x7E8 reply id (legacy connect_bootloader).
bytes::Bytes requestTo(std::uint32_t id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes requestTo(std::uint32_t id, std::initializer_list<bytes::Byte> payload)
{
    return requestTo(id, bytes::ByteView(payload.begin(), payload.size()));
}
bytes::Bytes request(bytes::ByteView payload)
{
    return requestTo(0x7e0, payload);
}
bytes::Bytes request(std::initializer_list<bytes::Byte> payload)
{
    return requestTo(0x7e0, payload);
}
bytes::Bytes responseFrom(std::uint32_t id, std::initializer_list<bytes::Byte> tail)
{
    return requestTo(id, tail);
}
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    return requestTo(0x7e8, tail);
}

fastecu::flash::FlashPlan readPlan()
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_denso_1n83m_1_5m_can_plan,
// so a plan whose operation the builder itself would refuse can still reach
// the executor -- the only way to prove the executor's own
// validate_subaru_denso_1n83m_1_5m_can_plan call rejects it before any I/O.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruDenso1n83m_1_5mCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kBlockStart, kBlockLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}};
    fields.image = bytes::Bytes(kImageSize, 0x00);
    fields.family_plan = SubaruDenso1n83m_1_5mCanPlan{0x7e0, 0x7e8, 500000, false, 0x10000, 0x100};
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt tables, transcribed independently from the same legacy
// lines the executor was
// (generate_can_seed_key/encrypt_payload/decrypt_payload) rather than read
// back from the executor's own translation unit, so a wrong table entry in the
// executor fails these assertions instead of passing silently.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                                      0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes seedKey(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

// The executor's decrypt table is this encrypt table exactly reversed, and
// SsmProtocol::calculatePayload's Feistel structure inverts by reversing key
// order, so this single helper both (a) pre-encrypts a known plaintext into
// the wire bytes a scripted read reply must carry for the executor's decrypt
// step to recover it, and (b) computes the wire bytes a write must carry for
// a known plaintext image.
bytes::Bytes toWire(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptTable,
                                         kIndexTransformation);
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// The OBK probe miss, the four non-fatal identity queries, the access-method
// probe and the branch selector. Byte 7 of the raw 0x22 0x10 0x1D reply frame
// -- payload index 3 -- selects the programming branch.
void scriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branchByte)
{
    const auto section = t.section("preliminaries");
    t.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));                         // OBK probe, miss
    t.exchange(request({0xAA}), response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));          // ECU ID
    t.exchange(request({0x09, 0x02}), response({0x49, 0x02, 'V', 'I', 'N'}));          // VIN
    t.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));          // CAL ID
    t.exchange(request({0x09, 0x06}), response({0x49, 0x06, 0xAA, 0xBB}));             // CVN
    t.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));                         // access method
    t.exchange(request({0x22, 0x10, 0x1D}), response({0x62, 0x10, 0x1D, branchByte})); // branch selector
}

// The bench arm.
void scriptBenchConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("bench connect");
    scriptPreliminaries(t, 0xFF);
    t.exchange(request({0x10, 0x43}), response({0x50, 0x43}));
    t.exchange(request({0x27, 0x61}), response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62};
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.exchange(request(key), response({0x67, 0x62}));
    t.exchange(request({0x10, 0x42}), response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory).
void scriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("read setup");
    t.exchange(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               response({0x74, 0x20, 0x01, 0x05}));
    t.exchange(request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               response({0x75, 0x20, 0x01, 0x01}));
}

// The chunked 0xB7 dump sweep (legacy read_memory): 0xB7 plus a 4-byte big-
// endian address, answered with 0xF7 plus one encrypted page.
void scriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length, std::uint32_t pagesize,
                     bytes::Byte fill)
{
    const auto section = t.section("flash dump");
    const bytes::Bytes wirePage = toWire(bytes::Bytes(pagesize, fill));
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        bytes::Bytes reply = response({0xF7});
        reply.insert(reply.end(), wirePage.begin(), wirePage.end());
        t.exchange(request(bytes::composeBe(bytes::Byte(0xB7), addr)), reply);
    }
}

// The 0x37 stop command (legacy read_memory).
void scriptStopCommand(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("stop command");
    t.exchange(request({0x37}), response({0x77}));
}

// Connect plus read_memory's 0x34 dump-setup request, registered as an
// expected write only -- its reply is left for the caller to queue, so the
// same script serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there.
void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    scriptBenchConnect(t);
    const auto section = t.section("read setup (first request only)");
    t.exchange(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}));
}

// The in-car arm. The ten fire-and-forget replies are deliberately given
// arbitration ids other than 0x7E8 wherever the addressed module would answer
// on its own id: legacy reads whichever frame arrives next without checking
// the id, and this pins that the port does not add a check legacy lacks.
void scriptInCarConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("in-car connect");
    scriptPreliminaries(t, 0x00);

    t.exchange(request({0x10, 0x5F}), response({0x50, 0x01})); // mismatch logs only

    t.exchange(requestTo(0x7A2, {0x10, 0xC0}), responseFrom(0x7AA, {0x50, 0xC0}));
    t.exchange(request({0x10, 0x63}), response({0x50, 0x63}));
    t.exchange(requestTo(0x7DF, {0x10, 0x03}), response({0x50, 0x03}));
    t.exchange(requestTo(0x7E1, {0x10, 0x63}), responseFrom(0x7E9, {0x50, 0x63}));
    t.exchange(requestTo(0x7B0, {0x10, 0x03}), responseFrom(0x7B8, {0x50, 0x03}));
    t.exchange(requestTo(0x7B0, {0x85, 0x02}), responseFrom(0x7B8, {0xC5, 0x02}));
    t.exchange(requestTo(0x7DF, {0x85, 0x02}), response({0xC5, 0x02}));
    t.exchange(requestTo(0x7B0, {0x85, 0x02}), responseFrom(0x7B8, {0xC5, 0x02}));
    t.exchange(requestTo(0x7DF, {0x85, 0x02}), response({0xC5, 0x02}));
    t.exchange(requestTo(0x7DF, {0x28, 0x03, 0x01}), response({0x68, 0x03}));

    t.exchange(request({0x27, 0x61}), response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62};
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.exchange(request(key), response({0x67, 0x62}));

    t.exchange(request({0x10, 0x5F}), response({0x50, 0x63}));                   // fatal on mismatch
    t.exchange(request({0x22, 0x10, 0x1D}), response({0x62, 0x10, 0x1D, 0x00})); // fatal on mismatch
    t.exchange(request({0x10, 0x62}), response({0x50, 0x62}));
}

// erase_memory's setup PDU plus its erase trigger; the trigger's answer is
// consumed by the re-read loop, not by a paired read.
void scriptEraseMemory(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("erase memory");
    t.exchange(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               response({0x74, 0x20, 0x01, 0x05}));
    t.exchange(request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block). `rom` is the
// whole 0x184000 plan image, encrypted once, and indexed from kImageStart --
// so chunk 0 carries encrypted[0x10000..0x10100).
void scriptReflashChunks(ScriptedCanFlashTransport& t, bytes::ByteView rom)
{
    const auto section = t.section("reflash chunks");
    const bytes::Bytes encrypted = toWire(rom);
    for (std::uint32_t offset = 0; offset < kBlockLength; offset += 256)
    {
        const std::uint32_t addr = kBlockStart + offset;
        t.exchange(request(bytes::composeBe(bytes::Byte(0xB6), addr,
                                            bytes::ByteView(encrypted).subspan(addr - kImageStart, 256))),
                   response({0xF6}));
    }
}

// The close-block 0x37 and the checksum verify. UdsClient absorbs the
// intermediate 0x78 responsePending NRC by re-reading, so only one write is
// expected even though two reads are queued.
void scriptCloseAndChecksum(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("close and checksum");
    t.exchange(request({0x37}), response({0x77}));
    t.exchange(request({0x31, 0x01, 0x02, 0x02, 0x01}), response({0x7F, 0x31, 0x78}));
    t.queueRead(response({0x71, 0x01, 0x02}));
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

TEST(SubaruDenso1n83m_1_5mCanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));                 // leading pad
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x10000, kBlockLength), Each(0xA5)); // decrypted payload
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));                    // tail pad
    EXPECT_TRUE(transport.scriptConsumed());
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
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_THAT(events.logs,
                testing::Contains(testing::Pair(fastecu::LogLevel::Info,
                                                "Connecting to ECU Denso 1N83M 1.5MB CAN bootloader, please wait...")));
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::Info,
                                                             "Reading ROM from ECU, Denso 1N83M 1.5MB using CAN")));
    EXPECT_THAT(events.logs, testing::Each(testing::Pair(testing::_, testing::Not(testing::HasSubstr("4MB")))));
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptInCarConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x10000, kBlockLength), Each(0x5A));
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, WriteErasesThenFlashesBlockOne)
{
    ScriptedCanFlashTransport transport;
    const bytes::Bytes rom = writeRom();

    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    transport.queueRead(response({0x71, 0x01, 0x02}));
    scriptReflashChunks(transport, rom);
    scriptCloseAndChecksum(transport);

    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
    // The write path's own identity line, the third of the three legacy named
    // the 4MB sibling in.
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::Info,
                                                             "Writing ROM to ECU, Denso 1N83M 1.5MB using CAN")));
    EXPECT_THAT(events.logs, testing::Each(testing::Pair(testing::_, testing::Not(testing::HasSubstr("4MB")))));
    // The image base is already pinned by the scripted exchanges:
    // scriptReflashChunks builds every 0xB6 frame from encrypted.subspan(addr
    // - kImageStart, 256). This adds only that calculatePayload is 4-byte-word
    // independent, so the page-at-a-time comparison above is a valid way to
    // express it.
    const bytes::Bytes encrypted = toWire(rom);
    EXPECT_THAT(bytes::Bytes(encrypted.begin() + 0x10000, encrypted.begin() + 0x10000 + 256),
                test_bytes::BytesEq(toWire(bytes::ByteView(rom).subspan(0x10000, 256))));
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
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, NegativeResponseDuringConnectFails)
{
    // The mirror of 1n83m_4m's tolerated checks: this family is strict, so a
    // negative response to the seed request, which returns STATUS_ERROR, must
    // abort rather than be logged and stepped over.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0xFF);
    transport.exchange(request({0x10, 0x43}), response({0x50, 0x43}));
    transport.exchange(request({0x27, 0x61}), response({0x7F, 0x27, 0x35}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
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
    scriptBenchConnect(transport);
    transport.exchange(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                       response({0x7F, 0x34, 0x31}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, EmptyBranchSelectorReplyFails)
{
    // An absent 0x22 0x10 0x1D reply is one of only two points in the
    // preliminary phase that return STATUS_ERROR.
    ScriptedCanFlashTransport transport;
    transport.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));
    transport.exchange(request({0xAA}), response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));
    transport.exchange(request({0x09, 0x02}), response({0x49, 0x02, 'V', 'I', 'N'}));
    transport.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));
    transport.exchange(request({0x09, 0x06}), response({0x49, 0x06, 0xAA, 0xBB}));
    transport.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));
    transport.exchange(request({0x22, 0x10, 0x1D}));
    transport.queue_no_frame();

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, EraseRetryExhaustionFails)
{
    // Legacy erase_memory's re-read loop: twenty reads, no re-send, then
    // "Flash area erase failed".
    ScriptedCanFlashTransport transport;
    const bytes::Bytes rom = writeRom();
    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.queueRead(response({0x71, 0x01, 0x03}));
    }

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
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
    static constexpr int kProbeCount = 9;

    static fastecu::flash::FlashPlan readPlan()
    {
        return ::readPlan();
    }

    static fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation)
    {
        return ::handBuiltPlan(operation);
    }

    static void scriptBenchConnect(ScriptedCanFlashTransport& t)
    {
        ::scriptBenchConnect(t);
    }

    static void scriptReadSetup(ScriptedCanFlashTransport& t)
    {
        ::scriptReadSetup(t);
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

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDenso1n83m_1_5mCan, CanExecutorConformance, Denso1n83m_1_5mCanTraits);

} // namespace
