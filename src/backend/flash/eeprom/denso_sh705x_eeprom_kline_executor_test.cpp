#include "src/backend/ports/testing/result_matchers.h"
// Equivalence + error-matrix tests for DensoSh705xEepromKlineExecutor,
// the portable replacement for the deleted
// EepromEcuSubaruDensoSH705xKlineOperation. Every literal byte sequence
// below is either transcribed directly from the legacy .cpp (see the
// comments citing exact line numbers, matching task-6-report.md's table) or
// computed at runtime via the same SsmProtocol helpers and hardcoded tables
// production used, matching the now-deleted characterization test's own
// approach (tests/test_eeprom_ecu_subaru_denso_sh705x_kline_operation_
// characterization.cpp, commit 4a91c02) -- nothing here is invented.
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/flash/testing/faulting_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Optional;

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::U24;
using namespace bytes::literals;

// eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp:62-63 -- tester_id =
// 0xF0, target_id = 0x10, hardcoded in execute() and mirrored by the
// builder's DensoSh705xEepromKlinePlan.
constexpr std::uint8_t kTesterId = 0xf0;
constexpr std::uint8_t kTargetId = 0x10;

// Matches the built-in catalog's
// sub_ecu_eeprom_denso_sh7055_kline entry (kernel_addr = 0xFFFF6004).
constexpr std::uint32_t kKernelStartAddr = 0xFFFF6004;

// ---- Request builders: TRANSCRIBE of each send_sid_* body (see
// task-6-report.md's table for exact legacy line ranges). ----

bytes::Bytes SidBfSsmInitRequest()
{
    return ssm_protocol::AddHeader(bytes::Bytes{0xbf}, kTesterId, kTargetId);
}
bytes::Bytes Sid81StartCommRequest()
{
    return ssm_protocol::AddHeader(bytes::Bytes{0x81}, kTesterId, kTargetId);
}
bytes::Bytes Sid83TimingsRequest()
{
    return ssm_protocol::AddHeader(bytes::Bytes{0x83, 0x00}, kTesterId, kTargetId);
}
bytes::Bytes Sid27RequestSeedRequest()
{
    return ssm_protocol::AddHeader(bytes::Bytes{0x27, 0x01}, kTesterId, kTargetId);
}
bytes::Bytes Sid27SendKeyRequest(bytes::ByteView key)
{
    return ssm_protocol::AddHeader(ComposeBe(0x27_b, 0x02_b, key), kTesterId, kTargetId);
}
bytes::Bytes Sid10StartDiagRequest()
{
    return ssm_protocol::AddHeader(bytes::Bytes{0x10, 0x85, 0x02}, kTesterId, kTargetId);
}
bytes::Bytes Sid34RequestUploadRequest(std::uint32_t dataaddr, std::uint32_t datalen)
{
    return ssm_protocol::AddHeader(ComposeBe(0x34_b, U24{dataaddr}, 0x04_b, U24{datalen}), kTesterId, kTargetId);
}
bytes::Bytes Sid36TransferDataRequest(std::uint32_t blockaddr, bytes::ByteView block_bytes)
{
    return ssm_protocol::AddHeader(ComposeBe(0x36_b, U24{blockaddr}, block_bytes), kTesterId, kTargetId);
}
bytes::Bytes Sid31StartRoutineRequest()
{
    return ssm_protocol::AddHeader(bytes::Bytes{0x31, 0x01, 0x01}, kTesterId, kTargetId);
}

// request_kernel_id(), lines 964-994: NOT addHeader-framed.
bytes::Bytes RequestKernelIdRequest()
{
    const auto frame = ComposeBeWithChecksum(bytes::Sum8, std::uint16_t{0xBEEF}, std::uint16_t{1}, 0x01_b);
    return {frame.begin(), frame.end()};
}

// Anchors three helpers against hardcoded wire bytes -- each became the same
// composeBe/composeBeWithChecksum expression as its production counterpart in
// denso_sh705x_eeprom_kline_executor.cpp, so a width or checksum-span bug
// there would move both sides together and hide behind a passing suite.
//
// requestKernelIdRequest(): [0xBE, 0xEF, 0x00, 0x01, 0x01] then sum8 checksum.
//   0xBE + 0xEF + 0x00 + 0x01 + 0x01 = 0x1AF -> & 0xFF = 0xAF.
TEST(DensoSh705xEepromKlineExecutorTest, RequestKernelIdRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(RequestKernelIdRequest(), ElementsAre(0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF));
}

// sid34RequestUploadRequest(0x001000, 0x000010): SsmProtocol::addHeader wraps
// the payload as [0x80][targetId][testerId][len][payload][checksum].
// Payload = [0x34, u24(0x001000), 0x04, u24(0x000010)]
//         = [0x34, 0x00, 0x10, 0x00, 0x04, 0x00, 0x00, 0x10] (8 bytes).
// Frame before checksum: [0x80, 0x10, 0xF0, 0x08, 0x34, 0x00, 0x10, 0x00,
//                          0x04, 0x00, 0x00, 0x10].
// Checksum (sum8): 0x80+0x10+0xF0+0x08+0x34+0x00+0x10+0x00+0x04+0x00+0x00+0x10
//   = 0x1E0 -> & 0xFF = 0xE0.
TEST(DensoSh705xEepromKlineExecutorTest, Sid34RequestUploadRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(Sid34RequestUploadRequest(0x001000, 0x000010),
                ElementsAre(0x80, 0x10, 0xF0, 0x08, 0x34, 0x00, 0x10, 0x00, 0x04, 0x00, 0x00, 0x10, 0xE0));
}

// sid27SendKeyRequest({0x11, 0x22}): payload = [0x27, 0x02, 0x11, 0x22] (4 bytes).
// Frame before checksum: [0x80, 0x10, 0xF0, 0x04, 0x27, 0x02, 0x11, 0x22].
// Checksum (sum8): 0x80+0x10+0xF0+0x04+0x27+0x02+0x11+0x22 = 0x1E0 -> & 0xFF = 0xE0.
TEST(DensoSh705xEepromKlineExecutorTest, Sid27SendKeyRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(Sid27SendKeyRequest(bytes::Bytes{0x11, 0x22}),
                ElementsAre(0x80, 0x10, 0xF0, 0x04, 0x27, 0x02, 0x11, 0x22, 0xE0));
}

// generate_seed_key(), lines 854-879 (stock / non-"_ecutek" table pair).
bytes::Bytes GenerateSeedKey(bytes::ByteView seed)
{
    static constexpr auto kIndex =
        std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608, 0xAFB8, 0x503D,
                                      0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0});
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kStockPairTable);
}

// generate_ecutek_seed_key(), lines 886-911: the same key table as stock,
// paired with the ECUTEK index transformation, which differs from the stock
// one in exactly its first five entries. Transcribed independently of
// production, so binding the wrong one of the two fails here.
bytes::Bytes GenerateEcutekSeedKey(bytes::ByteView seed)
{
    static constexpr auto kIndex =
        std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608, 0xAFB8, 0x503D,
                                      0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0});
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kEcutekPairTable);
}

// encrypt_payload(), lines 923-939.
bytes::Bytes EncryptPayload(bytes::ByteView buf)
{
    static constexpr auto kIndex = std::to_array<std::uint16_t>({0x7856, 0xCE22, 0xF513, 0x6E86});
    return ssm_protocol::CalculatePayload(buf, kIndex, ssm_protocol::kStockPairTable);
}

// For McuType "SH7055", kEepromBlocksSH7055[0] == {start=0, len=0x100}
// (src/backend/flash/kernel/kernelmemorymodels.h:279-281). read_mem()'s
// skip_start/willget/numblocks/curblock arithmetic (lines 469-510) reduces,
// for this start/length, to a single request with numblocks=8, curblock=0,
// for every EEPROM_MODE value.
bytes::Bytes SidDumpRequestForSh7055(std::uint8_t eeprom_mode)
{
    return {0xbd, eeprom_mode, 0x00, 0x08, 0x00, 0x00};
}

// read_mem()'s per-block unframing (lines 545-550): each 35-byte wire block
// is [2 prefix][32 data][1 trailing]; prefix/trailing are stripped and never
// inspected. 8 blocks * 35 bytes = 280 bytes.
bytes::Bytes EepromPayload280Bytes()
{
    bytes::Bytes out;
    for (int block = 0; block < 8; ++block)
    {
        out.push_back(0xEE);
        out.push_back(0xEE);
        std::ranges::transform(std::views::iota(0, 32), std::back_inserter(out),
                               [block](int j) { return static_cast<bytes::Byte>(block * 32 + j); });
        out.push_back(0xFF);
    }
    return out; // 280 bytes
}

bytes::Bytes ExpectedDecodedEeprom256Bytes()
{
    return std::views::iota(0, 256) | std::views::transform([](int i) { return static_cast<bytes::Byte>(i); }) |
           std::ranges::to<bytes::Bytes>();
}

// Minimal positive-response fixture: only byte index 4 (the service/session
// code) is inspected for these SIDs, so a 5-byte frame with that one byte
// set is sufficient and unambiguous with the "no frame"/too-short checks.
bytes::Bytes PositiveResponse(std::uint8_t service_code)
{
    bytes::Bytes out(5, 0);
    out[4] = service_code;
    return out;
}

// send_sid_bf_ssm_init()'s response: connect_bootloader() requires byte[4]
// == 0xFF; content beyond that is logged, never asserted, so 13 bytes of
// arbitrary filler (>= the 13 needed to avoid a negative-length strip in the
// legacy Qt code) is sufficient here too.
bytes::Bytes SidBfSsmInitResponse()
{
    bytes::Bytes out(13, 0);
    out[4] = 0xFF;
    out[8] = 'A';
    out[9] = 'B';
    out[10] = 'C';
    out[11] = 'D';
    out[12] = 'E';
    return out;
}

// send_sid_27_request_seed()'s response: byte[4] == 0x67, seed at [6..9].
bytes::Bytes Sid27SeedResponse(bytes::ByteView seed)
{
    bytes::Bytes out(10, 0);
    out[4] = 0x67;
    out[6] = seed[0];
    out[7] = seed[1];
    out[8] = seed[2];
    out[9] = seed[3];
    return out;
}

// request_kernel_id()'s "kernel is alive" response: received[0..1] ==
// 0xBEEF, received[4] == 0x01 | 0x40 == 0x41.
bytes::Bytes KernelAliveResponse()
{
    return ComposeBe(std::uint16_t{0xBEEF}, 0x00_b, 0x06_b, 0x41_b,
                     std::string_view("KERN2")); // 10 bytes
}

// A 16-byte (already 4-byte-aligned) kernel fixture -- matches
// send_sid_36_transferdata()'s single-block path exactly, and (per
// task-6-report.md's "Legacy behavior surprises" #1) is already aligned so
// this executor's OOB-read-avoidance padding is a documented no-op here.
bytes::Bytes KernelFixtureBytes()
{
    return std::views::iota(0, 16) | std::views::transform([](int i) { return static_cast<bytes::Byte>(i); }) |
           std::ranges::to<bytes::Bytes>();
}

// Enqueues the exact write/read sequence for one full "kernel not yet
// running" round: connect_bootloader()'s non-alive probe + full bf/81/83/
// 27/27/10 init, then upload_kernel()'s 34/36/34/36/31 sequence and its
// kernel-alive re-poll. 13 writes total.
void EnqueueFullBootloaderAndKernelUpload(ScriptedKlineFlashTransport& transport, bytes::ByteView seed,
                                          bytes::ByteView kernel_bytes, std::uint32_t kernel_start_addr)
{
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueNoFrame(); // kernel not (yet) alive

    transport.ExpectWrite(SidBfSsmInitRequest());
    transport.QueueRead(SidBfSsmInitResponse());

    transport.ExpectWrite(Sid81StartCommRequest());
    transport.QueueRead(PositiveResponse(0xC1));

    transport.ExpectWrite(Sid83TimingsRequest());
    transport.QueueRead(PositiveResponse(0xC3));

    transport.ExpectWrite(Sid27RequestSeedRequest());
    transport.QueueRead(Sid27SeedResponse(seed));

    const bytes::Bytes seed_key = GenerateSeedKey(seed);
    transport.ExpectWrite(Sid27SendKeyRequest(seed_key));
    transport.QueueRead(PositiveResponse(0x67));

    transport.ExpectWrite(Sid10StartDiagRequest());
    transport.QueueRead(PositiveResponse(0x50));

    const std::uint32_t pl_len = (static_cast<std::uint32_t>(kernel_bytes.size()) + 3) & ~std::uint32_t(3);
    transport.ExpectWrite(Sid34RequestUploadRequest(kernel_start_addr, pl_len));
    transport.QueueRead(PositiveResponse(0x74));

    // Pad to plLen with zero bytes before encrypting -- this is the
    // executor's OOB-read fix (denso_sh705x_eeprom_kline_executor.cpp,
    // upload_kernel()), not the legacy behavior. For an already-aligned
    // kernelBytes (kernelFixtureBytes(), used by most tests here) this is a
    // no-op; NonAlignedKernelIsPaddedBeforeEncryptionAvoidsOobRead below
    // exercises the case where padding actually changes the wire bytes.
    bytes::Bytes padded_kernel(kernel_bytes.begin(), kernel_bytes.end());
    padded_kernel.resize(pl_len, 0);
    transport.ExpectWrite(Sid36TransferDataRequest(kernel_start_addr, EncryptPayload(padded_kernel)));
    transport.QueueRead(PositiveResponse(0x76));

    const bytes::Bytes cks_bypass{0x00, 0x00, 0x5A, 0xA5};
    transport.ExpectWrite(Sid34RequestUploadRequest(kernel_start_addr + pl_len, 4));
    transport.QueueRead(PositiveResponse(0x74));

    transport.ExpectWrite(Sid36TransferDataRequest(kernel_start_addr + pl_len, EncryptPayload(cks_bypass)));
    transport.QueueRead(PositiveResponse(0x76));

    transport.ExpectWrite(Sid31StartRoutineRequest());
    transport.QueueRead(PositiveResponse(0x71));

    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueRead(KernelAliveResponse());
}

Result<FlashPlan> MakeKlinePlan(EepromReadMode mode, bytes::Bytes kernel_bytes, std::uint32_t kernel_addr,
                                DensoSecurityVariant security = DensoSecurityVariant::kStock)
{
    return BuildDensoSh705xEepromPlan(DensoSh705xEepromInput{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = kernel_addr, .bytes = std::move(kernel_bytes)},
        .mode = mode,
        .security = security,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
}

Result<FlashPlan> ValidKlinePlan(EepromReadMode mode = EepromReadMode::kMode2)
{
    return MakeKlinePlan(mode, KernelFixtureBytes(), kKernelStartAddr);
}

// The builder refuses this plan, so it reaches ValidateAndBuild as raw fields.
Result<FlashPlan> HandBuiltInvalidPlan()
{
    return ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0, .length = 0x100},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = kKernelStartAddr, .bytes = KernelFixtureBytes()},
        .family_plan = DensoSh705xEepromKlinePlan{.mode = EepromReadMode::kMode2,
                                                  .security = DensoSecurityVariant::kCobb,
                                                  .tester_id = 0xf0,
                                                  .target_id = 0x10,
                                                  .initial_baud = 4800,
                                                  .kernel_baud = 15625},
        .confirmations = {},
    });
}

} // namespace

TEST(DensoSh705xEepromKlineExecutorTest, TransportSetupReturnsPlansWireParameters)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    DensoSh705xEepromKlineExecutor executor;
    const auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 4800);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x10);
}

TEST(DensoSh705xEepromKlineExecutorTest, InvalidPlanIsRefusedBeforeAnyIo)
{
    const auto plan = HandBuiltInvalidPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    FakeCancellationToken cancellation;
    EXPECT_THAT(
        DensoSh705xEepromKlineExecutor{}.TransportSetup(*plan),
        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                    ::testing::Eq("security variant is not supported on the K-Line transport")));
    EXPECT_THAT(
        DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                    ::testing::Eq("security variant is not supported on the K-Line transport")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_THAT(transport.header_mode_calls, IsEmpty());
}

TEST(DensoSh705xEepromKlineExecutorTest, InvalidPlanWinsOverPrecancel)
{
    const auto plan = HandBuiltInvalidPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    FakeCancellationToken cancellation{true};
    EXPECT_THAT(
        DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                    ::testing::Eq("security variant is not supported on the K-Line transport")));
}

TEST(DensoSh705xEepromKlineExecutorTest, PrecancelledRunIsRefusedInAdmission)
{
    const auto plan = ValidKlinePlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    RecordingEventSink events;
    FakeCancellationToken cancellation{true};
    EXPECT_THAT(DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_THAT(transport.header_mode_calls, IsEmpty());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(DensoSh705xEepromKlineExecutorTest, FullBootloaderStockSecurityMode2MatchesLegacyTrace)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    EnqueueFullBootloaderAndKernelUpload(transport, seed, KernelFixtureBytes(), kKernelStartAddr);
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(*result->read_bytes, ExpectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(DensoSh705xEepromKlineExecutorTest, KernelAlreadyRunningSkipsBootloaderMatchesLegacyTrace)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueRead(KernelAliveResponse());
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(*result->read_bytes, ExpectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.ScriptConsumed());
}

// Proves the intentional OOB-read fix (see denso_sh705x_eeprom_kline_
// executor.cpp's upload_kernel()) is not just present but exercised: a
// 15-byte (non-4-aligned) kernel image rounds pl_len up to 16, and the
// executor pads the *source* buffer to 16 bytes (trailing zero) before
// encrypting, rather than reproducing legacy's OOB-prone
// encrypt_payload(<15-byte original buffer>, 16). If the executor didn't
// pad, this test's expected wire bytes (computed the same way) would not
// match, since SsmProtocol::calculatePayload() clamps its output length to
// the *unpadded* buffer size when len exceeds it.
TEST(DensoSh705xEepromKlineExecutorTest, NonAlignedKernelIsPaddedBeforeEncryptionAvoidsOobRead)
{
    auto kernel15 = std::views::iota(0, 15) | std::views::transform([](int i) { return static_cast<bytes::Byte>(i); }) |
                    std::ranges::to<bytes::Bytes>();
    auto plan = MakeKlinePlan(EepromReadMode::kMode2, kernel15, kKernelStartAddr);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    EnqueueFullBootloaderAndKernelUpload(transport, seed, kernel15, kKernelStartAddr);
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(*result->read_bytes, ExpectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(DensoSh705xEepromKlineExecutorTest, NoResponseAtHandshakeReturnsTimeout)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueNoFrame(); // kernel not (yet) alive
    transport.ExpectWrite(SidBfSsmInitRequest());
    transport.QueueNoFrame(); // no response at all -> Timeout

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
}

TEST(DensoSh705xEepromKlineExecutorTest, MalformedSid81ResponseReturnsBadResponse)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueNoFrame();
    transport.ExpectWrite(SidBfSsmInitRequest());
    transport.QueueRead(SidBfSsmInitResponse());
    transport.ExpectWrite(Sid81StartCommRequest());
    transport.QueueRead(PositiveResponse(0x00)); // byte[4] != 0xC1

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(DensoSh705xEepromKlineExecutorTest, CancellationDuringKernelUploadReturnsCancelled)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    EnqueueFullBootloaderAndKernelUpload(transport, seed, KernelFixtureBytes(), kKernelStartAddr);
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    // Trips partway through upload_kernel()'s chunk loop: connect_bootloader()
    // completes fully (kernel not alive -> probe + full bf/81/83/27/27/10
    // init = 7 writes), then upload_kernel() writes its kernel-upload
    // request (sid_34, write #8) before transfer_data_blocks()'s own
    // per-chunk cancellation guard trips -- the kernel-data chunk (sid_36,
    // what would be write #9) is never written. N tuned empirically (see
    // task-8-report.md) against this exact trace's total
    // cancellation.cancelled() call count, including the ones
    // FakeClock::sleep() performs internally.
    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(38);
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    // Concretely proves "between chunks": connect_bootloader's 7 writes
    // (probe + bf/81/83/27req/27key/10) plus upload_kernel's kernel-upload
    // request (sid_34) happened -- 8 total -- but the kernel-data chunk
    // (sid_36) was never written.
    EXPECT_EQ(transport.WritesConsumed(), 8U);
}

// Transport-seam regression test for the ISO14230 auto-header bug found in
// final whole-branch review: the legacy, now-deleted
// EepromEcuSubaruDensoSH705xKlineOperation::read_mem() called
// serial->set_add_iso14230_header(true) before its raw SID_DUMP request
// (connect/upload self-frame via SsmProtocol::addHeader() and need it OFF).
// The golden-trace equivalence tests above only assert bytes handed to
// transport.write()/read() -- they cannot see this, because the header-add
// happens one layer below that seam, inside the real serial driver. This
// test asserts the actual set_add_iso14230_header() call sequence relative
// to connect/upload/read, which is the only way to prove the seam exists and
// is used correctly.
TEST(DensoSh705xEepromKlineExecutorTest, HeaderModeIsOffForBootloaderOnForReadThenResetToOff)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    EnqueueFullBootloaderAndKernelUpload(transport, seed, KernelFixtureBytes(), kKernelStartAddr);
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(*result->read_bytes, ExpectedDecodedEeprom256Bytes());
    // false before connect_bootloader()/upload_kernel() (self-framed via
    // SsmProtocol::addHeader(), must not double-frame), true right before
    // read_mem()'s raw SID_DUMP requests, false again afterward so a shared,
    // session-lifetime serial instance isn't left dirtied for the next
    // unrelated operation.
    EXPECT_EQ(transport.header_mode_calls, (std::vector<bool>{false, true, false}));
}

// Same assertion on the "kernel already running" path (connect_bootloader()
// short-circuits, upload_kernel() is skipped entirely) -- proves the OFF ->
// ON -> OFF sequence doesn't depend on the upload phase actually running.
TEST(DensoSh705xEepromKlineExecutorTest, HeaderModeSequenceHoldsWhenKernelAlreadyRunning)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueRead(KernelAliveResponse());
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_EQ(transport.header_mode_calls, (std::vector<bool>{false, true, false}));
}

// Proves the header is still forced back OFF even when read_mem() itself
// fails (e.g. a hard transport error mid-read) -- the reset is unconditional
// on the result, not just on the happy path, since a stuck `true` on a
// shared, session-lifetime serial instance would corrupt every subsequent
// unrelated K-Line operation the user runs.
TEST(DensoSh705xEepromKlineExecutorTest, HeaderModeResetToOffEvenWhenReadMemFails)
{
    auto plan = ValidKlinePlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueRead(KernelAliveResponse());
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueError(ErrorKind::kDisconnected, "port dropped mid-read");

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport.header_mode_calls, (std::vector<bool>{false, true, false}));
}

// The stock and ECUTEK branches differ only in the index transformation they
// pass to calculateSeedKey, and those two tables are identical in 27 of their
// 32 entries. Before this test the K-Line suite never built an ECUTEK plan at
// all, so binding this branch to the stock table changed the key on the wire
// without failing anything. The CAN suite's
// AllFourSecurityVariantsProduceDistinctSeedKeyFrames is the equivalent guard
// on that side.
TEST(DensoSh705xEepromKlineExecutorTest, StockAndEcutekSecurityProduceDifferentSeedKeyFrames)
{
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};

    auto run_to_seed_key = [&](DensoSecurityVariant security, bytes::ByteView expected_key)
    {
        auto plan = MakeKlinePlan(EepromReadMode::kMode2, KernelFixtureBytes(), kKernelStartAddr, security);
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
        transport.ExpectWrite(RequestKernelIdRequest());
        transport.QueueNoFrame();
        transport.ExpectWrite(SidBfSsmInitRequest());
        transport.QueueRead(SidBfSsmInitResponse());
        transport.ExpectWrite(Sid81StartCommRequest());
        transport.QueueRead(PositiveResponse(0xC1));
        transport.ExpectWrite(Sid83TimingsRequest());
        transport.QueueRead(PositiveResponse(0xC3));
        transport.ExpectWrite(Sid27RequestSeedRequest());
        transport.QueueRead(Sid27SeedResponse(seed));
        // The executor's own key must match this byte for byte, or the
        // scripted write fails -- that is the assertion.
        transport.ExpectWrite(Sid27SendKeyRequest(expected_key));
        transport.QueueNoFrame(); // stop the round here

        DensoSh705xEepromKlineExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kTimeout));
        EXPECT_TRUE(transport.ScriptConsumed());
    };

    const bytes::Bytes stock_key = GenerateSeedKey(seed);
    const bytes::Bytes ecutek_key = GenerateEcutekSeedKey(seed);
    ASSERT_NE(stock_key, ecutek_key) << "the two transformations must not collapse to the same key";

    run_to_seed_key(DensoSecurityVariant::kStock, stock_key);
    run_to_seed_key(DensoSecurityVariant::kEcuTek, ecutek_key);
}

namespace
{
void EnqueueFullRead(ScriptedKlineFlashTransport& transport)
{
    EnqueueFullBootloaderAndKernelUpload(transport, bytes::Bytes{0x11, 0x22, 0x33, 0x44}, KernelFixtureBytes(),
                                         kKernelStartAddr);
    transport.ExpectWrite(SidDumpRequestForSh7055(2));
    transport.QueueRead(EepromPayload280Bytes());
}

// The number of `op` calls in a clean full read.
std::size_t CountCalls(KlineFaultOp op)
{
    auto plan = ValidKlinePlan();
    if (!plan.has_value())
    {
        ADD_FAILURE() << "plan rejected";
        return 0;
    }
    FaultingKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.op = op;
    EnqueueFullRead(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    std::ignore = DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events);
    return transport.calls;
}
} // namespace

TEST(DensoSh705xEepromKlineExecutorTest, EveryTransportFaultInAFullReadPropagatesAndStops)
{
    // The third header-mode call is the best-effort reset after the read; its
    // failure is logged, not returned.
    const std::vector<std::pair<KlineFaultOp, std::size_t>> ops{
        {KlineFaultOp::kWrite, CountCalls(KlineFaultOp::kWrite)},
        {KlineFaultOp::kRead, CountCalls(KlineFaultOp::kRead)},
        {KlineFaultOp::kSetBaud, CountCalls(KlineFaultOp::kSetBaud)},
        {KlineFaultOp::kHeaderMode, CountCalls(KlineFaultOp::kHeaderMode) - 1},
    };
    for (const auto& [op, count] : ops)
    {
        SCOPED_TRACE(::testing::Message() << "op " << static_cast<int>(op));
        ASSERT_GT(count, 0U);
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::Message() << "call " << k);
            auto plan = ValidKlinePlan();
            ASSERT_THAT(plan, fastecu::testing::IsOk());
            FaultingKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
            transport.op = op;
            transport.fail_at = k;
            EnqueueFullRead(transport);
            FakeClock clock;
            FakeCancellationToken cancellation;
            RecordingEventSink events;
            EXPECT_THAT(DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                        fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
            EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>{transport.WritesConsumed()});
        }
    }
}

TEST(DensoSh705xEepromKlineExecutorTest, EveryCancellationCheckInAFullReadStops)
{
    std::size_t checks = 0;
    {
        auto plan = ValidKlinePlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        EnqueueFullRead(transport);
        FakeClock clock;
        FakeCancellationToken cancellation;
        cancellation.SetPredicate(
            [&checks]
            {
                ++checks;
                return false;
            });
        RecordingEventSink events;
        ASSERT_THAT(DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsOk());
    }
    for (std::size_t n = 1; n <= checks; ++n)
    {
        SCOPED_TRACE(n);
        auto plan = ValidKlinePlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        EnqueueFullRead(transport);
        FakeClock clock;
        FakeCancellationToken cancellation;
        std::size_t seen = 0;
        std::optional<std::size_t> writes_at_cancel;
        cancellation.SetPredicate(
            [&]
            {
                if (++seen == n)
                {
                    writes_at_cancel = transport.WritesConsumed();
                }
                return seen >= n;
            });
        RecordingEventSink events;
        EXPECT_THAT(DensoSh705xEepromKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(writes_at_cancel, std::optional<std::size_t>{transport.WritesConsumed()});
    }
}

} // namespace fastecu::flash
