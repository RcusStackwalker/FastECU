#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::U24;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using namespace bytes::literals;
using namespace std::chrono_literals;

// ---- Wire transcription, independent of production helpers -------------

// Kernel frame: BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes Beef(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return ComposeBeWithChecksum(bytes::Sum8, std::uint16_t{0xBEEF}, std::uint16_t(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}
// A positive kernel reply: BE EF, length, opcode|0x40, data, sum8.
bytes::Bytes BeefReply(std::uint8_t opcode, bytes::ByteView data = {})
{
    return Beef(static_cast<std::uint8_t>(opcode | 0x40U), data);
}
// SSM request tester 0xF0 -> target 0x10: 80 10 F0 len payload sum8.
bytes::Bytes Ssm(bytes::ByteView payload)
{
    return ComposeBeWithChecksum(bytes::Sum8, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), payload);
}
// SSM reply target -> tester: 80 F0 10 len payload sum8.
bytes::Bytes SsmReply(bytes::ByteView payload)
{
    return ComposeBeWithChecksum(bytes::Sum8, 0x80_b, 0xF0_b, 0x10_b, bytes::Byte(payload.size()), payload);
}

constexpr auto kKeyTable =
    std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608, 0xAFB8, 0x503D,
                                  0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0});
constexpr auto kEncryptTable = std::to_array<std::uint16_t>({0x7856, 0xCE22, 0xF513, 0x6E86});

const bytes::Bytes kKernelIdRequest{0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF};
const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};
const bytes::Bytes kKernelBytes{0xAA, 0xBB, 0xCC, 0xDD};
// upload_kernel() transform of kKernelBytes, worked by hand in Task 5.
const bytes::Bytes kBalancedKernel{0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x8D, 0xC8};

bytes::Bytes KernelIdReply()
{
    return BeefReply(0x01, bytes::Bytes{'S', 'S', 'M', 'K'});
}

KernelImage KernelFor(std::string_view mcu)
{
    return KernelImage{.id = "k", .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U, .bytes = kKernelBytes};
}

FlashPlan MakePlan(FlashOperation operation, std::string_view protocol = "sub_ecu_denso_sh7055_04",
                   std::string_view mcu = "SH7055", std::optional<bytes::Bytes> image = std::nullopt)
{
    if (operation != FlashOperation::kRead && !image.has_value())
    {
        image = bytes::Bytes(FindFlashDevice(mcu)->romsize, 0xFF);
    }
    auto plan = BuildSubaruDensoSh705xKlinePlan(operation, protocol, mcu, std::move(image), KernelFor(mcu));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// connect_bootloader():127-157 -- kernel probe at 62500.
void ScriptProbeDead(ScriptedKlineFlashTransport& t)
{
    auto s = t.Section("probe");
    t.ExpectWrite(kKernelIdRequest);
    t.QueueNoFrame();
}
void ScriptProbeAlive(ScriptedKlineFlashTransport& t)
{
    auto s = t.Section("probe");
    t.Exchange(kKernelIdRequest, KernelIdReply());
}

bytes::Bytes StockKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kKeyTable, ssm_protocol::kIndexTransformationStock);
}
bytes::Bytes EcutekKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kKeyTable, ssm_protocol::kIndexTransformationEcutek);
}
bytes::Bytes EncryptedKernel(bytes::ByteView balanced)
{
    return ssm_protocol::CalculatePayload(balanced, static_cast<std::uint32_t>(balanced.size()), kEncryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

const bytes::Bytes kEcuIdPayload{0xFF, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44, 0x45};

// connect_bootloader():161-323 -- SSM handshake at 4800.
void ScriptHandshake(ScriptedKlineFlashTransport& t, bytes::ByteView key)
{
    auto s = t.Section("handshake");
    t.Exchange(Ssm(bytes::Bytes{0xBF}), SsmReply(kEcuIdPayload));
    t.Exchange(Ssm(bytes::Bytes{0x81}), SsmReply(bytes::Bytes{0xC1}));
    t.Exchange(Ssm(bytes::Bytes{0x83, 0x00}), SsmReply(bytes::Bytes{0xC3}));
    t.Exchange(Ssm(bytes::Bytes{0x27, 0x01}), SsmReply(ComposeBe(0x67_b, 0x01_b, kSeed)));
    t.Exchange(Ssm(ComposeBe(0x27_b, 0x02_b, key)), SsmReply(bytes::Bytes{0x67, 0x02}));
    t.Exchange(Ssm(bytes::Bytes{0x10, 0x85, 0x02}), SsmReply(bytes::Bytes{0x50}));
}

// upload_kernel():354-460 -- 34 / 36 / 31 at 15625.
void ScriptUploadFrames(ScriptedKlineFlashTransport& t, std::uint32_t address)
{
    const bytes::Bytes encrypted = EncryptedKernel(kBalancedKernel);
    t.Exchange(Ssm(ComposeBe(0x34_b, U24(address), 0x04_b, U24(8))), SsmReply(bytes::Bytes{0x74}));
    t.Exchange(Ssm(ComposeBe(0x36_b, U24(address), encrypted)), SsmReply(bytes::Bytes{0x76}));
    t.Exchange(Ssm(bytes::Bytes{0x31, 0x01, 0x01}), SsmReply(bytes::Bytes{0x71}));
}

// upload_kernel():354-496 -- 34 / 36 / 31 at 15625, then kernel ID at 62500.
void ScriptUpload(ScriptedKlineFlashTransport& t, std::uint32_t address)
{
    auto s = t.Section("upload");
    ScriptUploadFrames(t, address);
    t.Exchange(kKernelIdRequest, KernelIdReply());
}

void ScriptSession(ScriptedKlineFlashTransport& t, bytes::ByteView key = {}, std::uint32_t address = 0xFFFF6004)
{
    ScriptProbeDead(t);
    ScriptHandshake(t, key.empty() ? StockKey(kSeed) : bytes::Bytes(key.begin(), key.end()));
    ScriptUpload(t, address);
}

// check_romcrc():812-831 -- CRC [addr32, 00, len24] -> BE EF len 42 crc32 sum8.
bytes::Bytes CrcRequest(const FlashBlock& block)
{
    return Beef(0x02, ComposeBe(block.start, 0x00_b, U24(block.len)));
}

// The session tests below pin only the session; the Write/TestWrite tail's
// first frame is the block-0 CRC request, and a transport error on its reply
// stops the tail deterministically before any further frame.
void ScriptStopAtFirstCrc(ScriptedKlineFlashTransport& t, std::string_view mcu = "SH7055")
{
    auto s = t.Section("stop at first CRC");
    t.ExpectWrite(CrcRequest(FindFlashDevice(mcu)->fblocks[0]));
    t.QueueError(ErrorKind::kDisconnected, "stop after session");
}

struct Harness
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    Result<FlashExecutionResult> Run(const FlashPlan& plan)
    {
        return executor.Execute(plan, transport, clock, cancellation, events);
    }
};

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelMatchesHandWorkedVectors)
{
    // AA BB CC DD +00 00 -> pad to 8 -> drop 2 -> AA BB CC DD 00 00.
    // Words (bytes past the end read as zero): AABBCCDD, 00000000.
    // u16 sum = 0xCCDD; 0x5AA5 - 0xCCDD = 0x8DC8 (mod 2^16).
    EXPECT_EQ(DensoSh705xKlineBalancedKernel(kKernelBytes), kBalancedKernel);
    // 01 02 03 +00 00 -> 01 02 03 00 00 00 00 00 -> drop 2 -> 6 bytes.
    // u16 sum = 0x0300; balance = 0x57A5.
    EXPECT_EQ(DensoSh705xKlineBalancedKernel(bytes::Bytes{0x01, 0x02, 0x03}),
              (bytes::Bytes{0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x57, 0xA5}));
}

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelWordSumIsAlways5AA5)
{
    for (std::size_t size = 1; size <= 9; ++size)
    {
        SCOPED_TRACE(size);
        bytes::Bytes kernel(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            kernel[i] = static_cast<bytes::Byte>(0x31 * (i + 1));
        }
        const bytes::Bytes out = DensoSh705xKlineBalancedKernel(kernel);
        ASSERT_EQ(out.size() % 4, 0U);
        std::uint16_t sum = 0;
        for (std::size_t i = 0; i < out.size(); i += 4)
        {
            sum = static_cast<std::uint16_t>(sum + bytes::ReadU32Be(out, i));
        }
        EXPECT_EQ(sum, 0x5AA5);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, TransportSetupIsNonIso14230At4800)
{
    SubaruDensoSh705xKlineExecutor executor;
    const auto config = executor.TransportSetup(MakePlan(FlashOperation::kRead));
    ASSERT_THAT(config, IsOk());
    EXPECT_EQ(config->baud, 4800);
    EXPECT_FALSE(config->iso14230);
    EXPECT_EQ(config->tester_id, 0xF0);
    EXPECT_EQ(config->target_id, 0x10);
    EXPECT_EQ(config->parity, KlineParity::kNone);
}

TEST(SubaruDensoSh705xKlineExecutor, BoundAttemptResetsBeforeConfigure)
{
    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    auto *observed = transport.get();
    observed->set_baud_result = Fail(ErrorKind::kDisconnected, "stop after lifecycle");
    auto attempt = BindFlashAttempt(MakePlan(FlashOperation::kRead), std::make_unique<SubaruDensoSh705xKlineExecutor>(),
                                    std::move(transport));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    // The first setBaud (connect_bootloader():127, 62500) fails and stops execute().
    EXPECT_THAT(attempt->Run(clock, cancellation, events), IsErr(ErrorKind::kDisconnected));
    // execute():67 reset_connection() precedes every setter and open_serial_port().
    EXPECT_THAT(observed->lifecycle_calls, ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(SubaruDensoSh705xKlineExecutor, CancellationAroundResetStopsBeforeConfigure)
{
    for (const std::size_t check : {1U, 2U})
    {
        SCOPED_TRACE(check);
        ScriptedKlineFlashTransport transport;
        FakeClock clock;
        FakeCancellationToken cancellation;
        cancellation.CancelOnCheck(check);
        SubaruDensoSh705xKlineExecutor executor;

        EXPECT_THAT(executor.BeforeTransportConfigure(transport, clock, cancellation), IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(transport.reset_call_count, check == 1 ? 0 : 1);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ResetFailurePropagates)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result = Fail(ErrorKind::kDisconnected, "no adapter");
    FakeClock clock;
    FakeCancellationToken cancellation;
    SubaruDensoSh705xKlineExecutor executor;

    EXPECT_THAT(executor.BeforeTransportConfigure(transport, clock, cancellation), IsErr(ErrorKind::kDisconnected));
}

TEST(SubaruDensoSh705xKlineExecutor, RejectsAForeignPlanBeforeIo)
{
    Harness h;
    FlashPlanFields fields{.operation = FlashOperation::kRead,
                           .family = FlashFamily::kSubaruUnisiaJecs,
                           .transport = TransportKind::kKline,
                           .target_id = "sub_ecu_unisia_jecs_m3779x",
                           .mcu_name = "M3779x",
                           .transfer_region = {0, 0x10000},
                           .erase_regions = {},
                           .image = std::nullopt,
                           .kernel = std::nullopt,
                           .family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true},
                           .confirmations = {}};
    auto foreign = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(foreign, IsOk());

    EXPECT_THAT(h.executor.TransportSetup(*foreign), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(h.Run(*foreign), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(h.transport.WritesConsumed(), 0U);
}

// ---- Session: probe, handshake, upload (Task 6) -------------------------
// The operation tail is still a stub; these pin the transcript, not its kind.

TEST(SubaruDensoSh705xKlineExecutor, FullSessionIsByteExactWithLegacyBaudsAndTimeouts)
{
    // TestWrite, stopped at the tail's first frame (the block-0 CRC request).
    Harness h;
    ScriptSession(h.transport);
    ScriptStopAtFirstCrc(h.transport);

    ASSERT_THAT(h.Run(MakePlan(FlashOperation::kTestWrite)), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(h.transport.ScriptConsumed());
    // execute():68 -- the header flag is cleared once, before any frame.
    EXPECT_EQ(h.transport.header_mode_calls, std::vector<bool>{false});
    EXPECT_THAT(h.transport.baud_calls, ::testing::ElementsAre(62500, 4800, 15625, 62500));
    // probe 800, BF..10 six x 2000, 34 3000, 36 2000, 31 3000, kernel ID 800,
    // then the tail's block-0 CRC read at 3000 that stops the run.
    EXPECT_THAT(h.transport.read_timeouts, ::testing::ElementsAre(800ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms,
                                                                  3000ms, 2000ms, 3000ms, 800ms, 3000ms));
    // delay(100) after 62500, delay(200) in request_kernel_id(), delay(100)
    // after 4800, delay(100) after 31, delay(200) in request_kernel_id().
    EXPECT_EQ(h.clock.Elapsed(), 700ms);
}

TEST(SubaruDensoSh705xKlineExecutor, HeaderFlagFailurePropagatesBeforeAnyWrite)
{
    // execute():68 -- set_add_iso14230_header(false) precedes the first setBaud and write.
    Harness h;
    h.transport.set_add_iso14230_header_result = Fail(ErrorKind::kDisconnected, "header flag");

    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(h.transport.header_mode_calls, std::vector<bool>{false});
    EXPECT_TRUE(h.transport.baud_calls.empty());
    EXPECT_EQ(h.transport.WritesConsumed(), 0U);
}

TEST(SubaruDensoSh705xKlineExecutor, SessionLogsTheLegacyStrings)
{
    // TestWrite: this test pins the session transcript, not the operation
    // kind. The tail is stopped at its first frame (the block-0 CRC request)
    // and only the session's prefix of the log is compared.
    Harness h;
    ScriptSession(h.transport);
    ScriptStopAtFirstCrc(h.transport);
    ASSERT_THAT(h.Run(MakePlan(FlashOperation::kTestWrite)), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(h.transport.ScriptConsumed());

    using L = LogLevel;
    const std::vector<std::pair<LogLevel, std::string>> expected{
        {L::kInfo, "Connecting to Subaru 04 32-bit K-Line bootloader, please wait..."},
        {L::kInfo, "Checking if kernel is already running..."},
        {L::kInfo, "Requesting kernel ID"},
        {L::kError, "No valid response from ECU"},
        {L::kInfo, "No response from kernel, initialising ECU..."},
        {L::kInfo, "Requesting ECU ID"},
        {L::kInfo, "ECU ID: 4142434445"},
        {L::kInfo, "Requesting to start communication"},
        {L::kInfo, "Start communication ok"},
        {L::kInfo, "Requesting timings params"},
        {L::kInfo, "Timing parameters ok"},
        {L::kInfo, "Requesting seed"},
        {L::kInfo, "Seed request ok"},
        {L::kInfo, "Received seed: 11 22 33 44 "},
        {L::kInfo, "Calculated seed key: " + bytes::ToHex(StockKey(kSeed))},
        {L::kInfo, "Sending seed key to ECU"},
        {L::kInfo, "Seed key ok"},
        {L::kInfo, "Set session mode"},
        {L::kInfo, "Succesfully set to programming session"},
        {L::kInfo, "Initializing Subaru 04 32-bit K-Line kernel upload, please wait..."},
        {L::kDebug, "Start address to upload kernel: 0xffff6004"},
        {L::kInfo, "Requesting kernel upload"},
        {L::kInfo, "Kernel upload request ok"},
        {L::kInfo, "Transfer kernel data"},
        {L::kInfo, "Kernel uploaded"},
        {L::kInfo, "Jump to kernel"},
        {L::kInfo, "Kernel started, initializing..."},
        {L::kInfo, "Requesting kernel ID"},
        {L::kInfo, "Kernel ID: SSMK"},
    };
    ASSERT_GE(h.events.logs.size(), expected.size());
    EXPECT_EQ(std::vector(h.events.logs.begin(), h.events.logs.begin() + static_cast<std::ptrdiff_t>(expected.size())),
              expected);
    // execute():84 during the session, then :98 once the write tail starts.
    EXPECT_THAT(h.events.notices, ::testing::ElementsAre("Preparing, please wait...", "Writing ROM, please wait..."));
}

TEST(SubaruDensoSh705xKlineExecutor, EcutekProtocolSendsTheEcutekKey)
{
    Harness h;
    ScriptSession(h.transport, EcutekKey(kSeed));
    ASSERT_NE(EcutekKey(kSeed), StockKey(kSeed));
    EXPECT_FALSE(h.Run(MakePlan(FlashOperation::kRead, "sub_ecu_denso_sh7055_04_ecutek")).has_value());
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, Sh7058UploadsToItsOwnKernelAddress)
{
    Harness h;
    ScriptSession(h.transport, {}, 0xFFFF3000);
    EXPECT_FALSE(h.Run(MakePlan(FlashOperation::kRead, "sub_ecu_denso_sh7058", "SH7058")).has_value());
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, KernelTransferSplitsInto0x80ByteBlocks)
{
    // send_sid_36_transferdata():1533-1566 -- 0x80-byte blocks at addr +
    // blockno * 0x80; the last block carries the remainder.
    const bytes::Bytes kernel(0x100, 0x5A);
    const bytes::Bytes balanced = DensoSh705xKlineBalancedKernel(kernel);
    ASSERT_EQ(balanced.size(), 0x104U);
    const bytes::Bytes encrypted = EncryptedKernel(balanced);
    const bytes::ByteView view(encrypted);

    Harness h;
    ScriptProbeDead(h.transport);
    ScriptHandshake(h.transport, StockKey(kSeed));
    {
        auto s = h.transport.Section("upload");
        h.transport.Exchange(Ssm(ComposeBe(0x34_b, U24(0xFFFF6004), 0x04_b, U24(0x104))), SsmReply(bytes::Bytes{0x74}));
        h.transport.Exchange(Ssm(ComposeBe(0x36_b, U24(0xFFFF6004), view.subspan(0, 0x80))),
                             SsmReply(bytes::Bytes{0x76}));
        h.transport.Exchange(Ssm(ComposeBe(0x36_b, U24(0xFFFF6084), view.subspan(0x80, 0x80))),
                             SsmReply(bytes::Bytes{0x76}));
        h.transport.Exchange(Ssm(ComposeBe(0x36_b, U24(0xFFFF6104), view.subspan(0x100, 4))),
                             SsmReply(bytes::Bytes{0x76}));
        h.transport.Exchange(Ssm(bytes::Bytes{0x31, 0x01, 0x01}), SsmReply(bytes::Bytes{0x71}));
        h.transport.Exchange(kKernelIdRequest, KernelIdReply());
    }
    ScriptStopAtFirstCrc(h.transport);
    // TestWrite: this test pins the kernel-transfer chunking, not the
    // operation kind. The tail stops at its first CRC request, before
    // write_mem() reports any progress of its own.
    auto plan = BuildSubaruDensoSh705xKlinePlan(FlashOperation::kTestWrite, "sub_ecu_denso_sh7055_04", "SH7055",
                                                bytes::Bytes(FindFlashDevice("SH7055")->romsize, 0xFF),
                                                KernelImage{.id = "k", .load_address = 0xFFFF6004U, .bytes = kernel});
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(h.Run(*plan), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(h.transport.ScriptConsumed());
    EXPECT_THAT(h.events.progress_calls, ::testing::ElementsAre(std::pair{0, 0x104}, std::pair{0x80, 0x104},
                                                                std::pair{0x100, 0x104}, std::pair{0x104, 0x104}));
}

TEST(SubaruDensoSh705xKlineExecutor, RejectedKernelTransferBlockStops)
{
    Harness h;
    ScriptProbeDead(h.transport);
    ScriptHandshake(h.transport, StockKey(kSeed));
    h.transport.Exchange(Ssm(ComposeBe(0x34_b, U24(0xFFFF6004), 0x04_b, U24(8))), SsmReply(bytes::Bytes{0x74}));
    h.transport.Exchange(Ssm(ComposeBe(0x36_b, U24(0xFFFF6004), EncryptedKernel(kBalancedKernel))),
                         SsmReply(bytes::Bytes{0x7F, 0x36, 0x22}));
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, LiveKernelSkipsHandshakeAndUpload)
{
    // TestWrite: pins that a live kernel skips the handshake/upload, not the
    // operation kind. The tail stops at its first CRC request; its own
    // "Writing ROM, please wait..." notice is the only one, so the session's
    // "Preparing, please wait..." is absent.
    Harness h;
    ScriptProbeAlive(h.transport);
    ScriptStopAtFirstCrc(h.transport);
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kTestWrite)), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(h.transport.ScriptConsumed());
    EXPECT_THAT(h.transport.baud_calls, ::testing::ElementsAre(62500));
    EXPECT_THAT(h.events.notices, ::testing::ElementsAre("Writing ROM, please wait..."));
}

TEST(SubaruDensoSh705xKlineExecutor, MinimalLiveKernelReplyHasAnEmptyId)
{
    // A 5-byte reply passes legacy's `> 4` check; remove(0, 5) leaves nothing.
    // TestWrite: this test pins the probe's own logging, not the operation
    // kind. The tail stops at its first CRC request; the probe's last log
    // is the one immediately before the tail's first log.
    Harness h;
    {
        auto s = h.transport.Section("probe");
        h.transport.Exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x41});
    }
    ScriptStopAtFirstCrc(h.transport);
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kTestWrite)), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(h.transport.ScriptConsumed());
    const std::pair<LogLevel, std::string> tail_start{LogLevel::kInfo, "Writing ROM to Subaru 04 32-bit using K-Line"};
    const auto tail = std::ranges::find(h.events.logs, tail_start);
    ASSERT_NE(tail, h.events.logs.begin());
    ASSERT_NE(tail, h.events.logs.end());
    EXPECT_EQ(*std::prev(tail), (std::pair<LogLevel, std::string>{LogLevel::kInfo, "Kernel ID: "}));
}

TEST(SubaruDensoSh705xKlineExecutor, WrongProbeReplyFallsThroughToHandshake)
{
    Harness h;
    {
        auto s = h.transport.Section("probe");
        h.transport.Exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    }
    ScriptHandshake(h.transport, StockKey(kSeed));
    ScriptUpload(h.transport, 0xFFFF6004);
    EXPECT_FALSE(h.Run(MakePlan(FlashOperation::kRead)).has_value());
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, ShortEcuIdReplyIsRejectedBeforeSlicing)
{
    // Correction: legacy removed 8 bytes after checking only `> 4`.
    Harness h;
    ScriptProbeDead(h.transport);
    h.transport.Exchange(Ssm(bytes::Bytes{0xBF}), SsmReply(bytes::Bytes{0xFF, 0x00}));
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, EachNegativeHandshakeReplyStopsTheSession)
{
    struct Case
    {
        int step; // 0 = BF ... 5 = 10
        bytes::Bytes reply_payload;
    };
    const std::vector<Case> cases{{0, {0x7F, 0xBF}}, {1, {0x7F, 0x81}},
                                  {2, {0x7F, 0x83}}, {3, {0x67, 0x02, 0x11, 0x22, 0x33, 0x44}},
                                  {4, {0x67, 0x01}}, {5, {0x7F, 0x10}}};
    const std::vector<bytes::Bytes> requests{Ssm(bytes::Bytes{0xBF}),
                                             Ssm(bytes::Bytes{0x81}),
                                             Ssm(bytes::Bytes{0x83, 0x00}),
                                             Ssm(bytes::Bytes{0x27, 0x01}),
                                             Ssm(ComposeBe(0x27_b, 0x02_b, StockKey(kSeed))),
                                             Ssm(bytes::Bytes{0x10, 0x85, 0x02})};
    const std::vector<bytes::Bytes> good{SsmReply(kEcuIdPayload),
                                         SsmReply(bytes::Bytes{0xC1}),
                                         SsmReply(bytes::Bytes{0xC3}),
                                         SsmReply(ComposeBe(0x67_b, 0x01_b, kSeed)),
                                         SsmReply(bytes::Bytes{0x67, 0x02}),
                                         SsmReply(bytes::Bytes{0x50})};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.step);
        Harness h;
        ScriptProbeDead(h.transport);
        for (int i = 0; i < c.step; ++i)
        {
            h.transport.Exchange(requests[i], good[i]);
        }
        h.transport.Exchange(requests[c.step], SsmReply(c.reply_payload));
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(h.transport.ScriptConsumed()); // nothing sent after the bad reply
    }
}

TEST(SubaruDensoSh705xKlineExecutor, NegativeHandshakeReplyLogsTheNrc)
{
    // connect_bootloader():170 -- "Wrong response from ECU: " + parse_nrc_message(mid(4)).
    Harness h;
    ScriptProbeDead(h.transport);
    h.transport.Exchange(Ssm(bytes::Bytes{0xBF}), SsmReply(bytes::Bytes{0x7F, 0xBF, 0x12}));
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kBadResponse));
    ASSERT_FALSE(h.events.logs.empty());
    EXPECT_EQ(h.events.logs.back().first, LogLevel::kError);
    EXPECT_TRUE(h.events.logs.back().second.starts_with("Wrong response from ECU: "));
    EXPECT_NE(h.events.logs.back().second, "Wrong response from ECU: Not a valid answer");
}

TEST(SubaruDensoSh705xKlineExecutor, SilentHandshakeStepIsATimeout)
{
    Harness h;
    ScriptProbeDead(h.transport);
    h.transport.ExpectWrite(Ssm(bytes::Bytes{0xBF}));
    h.transport.QueueNoFrame();
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kTimeout));
}

TEST(SubaruDensoSh705xKlineExecutor, FailedUploadBaudChangeStops)
{
    // setBaud results are shared across calls; fail from the third call on.
    struct FailThirdBaud final : ScriptedKlineFlashTransport
    {
        using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;
        Status SetBaud(int baud) override
        {
            baud_calls.push_back(baud);
            return baud_calls.size() >= 3 ? Fail(ErrorKind::kDisconnected, "baud") : Status{};
        }
    };
    FailThirdBaud t{ScriptedTransportInitialState::kOpen};
    ScriptProbeDead(t);
    ScriptHandshake(t, StockKey(kSeed));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;
    EXPECT_THAT(executor.Execute(MakePlan(FlashOperation::kRead), t, clock, cancellation, events),
                IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(t.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, FinalBaudChangeFailureIsNowChecked)
{
    // Correction: legacy ignored upload_kernel()'s change_port_speed("62500").
    struct FailFourthBaud final : ScriptedKlineFlashTransport
    {
        using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;
        Status SetBaud(int baud) override
        {
            baud_calls.push_back(baud);
            return baud_calls.size() == 4 ? Fail(ErrorKind::kDisconnected, "baud") : Status{};
        }
    };
    FailFourthBaud t{ScriptedTransportInitialState::kOpen};
    ScriptProbeDead(t);
    ScriptHandshake(t, StockKey(kSeed));
    {
        auto s = t.Section("upload without kernel ID");
        ScriptUploadFrames(t, 0xFFFF6004);
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;
    EXPECT_THAT(executor.Execute(MakePlan(FlashOperation::kRead), t, clock, cancellation, events),
                IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(t.ScriptConsumed()); // no kernel-ID request after the failed baud change
}

TEST(SubaruDensoSh705xKlineExecutor, DeadKernelAfterUploadIsABadResponse)
{
    Harness h;
    ScriptProbeDead(h.transport);
    ScriptHandshake(h.transport, StockKey(kSeed));
    {
        auto s = h.transport.Section("upload");
        ScriptUploadFrames(h.transport, 0xFFFF6004);
        h.transport.Exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    }
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, SilentKernelAfterUploadIsATimeout)
{
    Harness h;
    ScriptProbeDead(h.transport);
    ScriptHandshake(h.transport, StockKey(kSeed));
    {
        auto s = h.transport.Section("upload");
        ScriptUploadFrames(h.transport, 0xFFFF6004);
        h.transport.ExpectWrite(kKernelIdRequest);
        h.transport.QueueNoFrame();
    }
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(h.transport.ScriptConsumed());
}

// The session writes 11 frames: probe 1, handshake 6, 34/36/31 3, kernel ID 1.
constexpr std::size_t kSessionWrites = 11;

TEST(SubaruDensoSh705xKlineExecutor, CancellationBeforeAnySessionWriteSendsNothingFurther)
{
    // Keyed on writes, not check numbers, so it stays valid as tasks 7-8
    // extend execute(): cancelling once k frames are out must send no more.
    for (std::size_t k = 0; k < kSessionWrites; ++k)
    {
        SCOPED_TRACE(k);
        Harness h;
        ScriptSession(h.transport);
        h.cancellation.SetPredicate([&h, k] { return h.transport.WritesConsumed() >= k; });
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(h.transport.WritesConsumed(), k);
    }
}

// ---- Read (Task 7) -------------------------------------------------------

// read_mem(): READ_AREA [00, addr24, 0x0400], reply BE EF len 43 <0x400 bytes> sum8.
bytes::Bytes ReadRequest(std::uint32_t address)
{
    return Beef(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{0x0400}));
}
bytes::Bytes PageReply(std::uint32_t address)
{
    bytes::Bytes data(0x400);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<bytes::Byte>((address >> 10U) + i);
    }
    return BeefReply(0x03, data);
}

TEST(SubaruDensoSh705xKlineExecutor, ReadReturnsTheWholeRomAndTheRomId)
{
    Harness h;
    ScriptSession(h.transport);
    const std::uint32_t romsize = FindFlashDevice("SH7055")->romsize;
    bytes::Bytes expected;
    for (std::uint32_t address = 0; address < romsize; address += 0x400)
    {
        h.transport.Exchange(ReadRequest(address), PageReply(address));
        const bytes::Bytes reply = PageReply(address);
        expected.insert(expected.end(), reply.begin() + 5, reply.end() - 1);
    }

    const auto result = h.Run(MakePlan(FlashOperation::kRead));

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(h.transport.ScriptConsumed());
    EXPECT_EQ(result->read_bytes, expected);
    // execute()/connect_bootloader():206 RomId = ecuid + "_".
    EXPECT_EQ(result->rom_id, std::optional<std::string>("4142434445_"));
    // execute():92-93 -- externalLoggerMessage() then LOG_I() before read_mem().
    EXPECT_THAT(h.events.notices, ::testing::Contains("Reading ROM, please wait..."));
    // read_mem():523 and :628 bracket the page loop.
    using L = LogLevel;
    const auto& logs = h.events.logs;
    const auto start = std::ranges::find(
        logs, std::pair<LogLevel, std::string>{L::kInfo, "Reading ROM from Subaru 04 32-bit using K-Line"});
    ASSERT_NE(start, logs.end());
    EXPECT_THAT(std::vector(start, logs.end()),
                ::testing::ElementsAre(::testing::Pair(L::kInfo, "Reading ROM from Subaru 04 32-bit using K-Line"),
                                       ::testing::Pair(L::kInfo, "Start reading ROM, please wait..."),
                                       ::testing::Pair(L::kInfo, "ROM read ready")));
}

TEST(SubaruDensoSh705xKlineExecutor, ReadWithLiveKernelHasNoRomId)
{
    Harness h;
    ScriptProbeAlive(h.transport);
    const std::uint32_t romsize = FindFlashDevice("SH7055")->romsize;
    for (std::uint32_t address = 0; address < romsize; address += 0x400)
    {
        h.transport.Exchange(ReadRequest(address), PageReply(address));
    }
    const auto result = h.Run(MakePlan(FlashOperation::kRead));
    ASSERT_THAT(result, IsOk());
    EXPECT_FALSE(result->rom_id.has_value());
}

TEST(SubaruDensoSh705xKlineExecutor, ReadRejectsShortBadChecksumAndWrongOpcodePages)
{
    bytes::Bytes short_page = PageReply(0);
    short_page.erase(short_page.begin() + 10); // one data byte missing
    bytes::Bytes bad_sum = PageReply(0);
    bad_sum.back() ^= 0x01U;
    bytes::Bytes wrong_op = PageReply(0);
    wrong_op[4] = 0x7F;
    for (const bytes::Bytes& reply : {short_page, bad_sum, wrong_op})
    {
        Harness h;
        ScriptSession(h.transport);
        h.transport.Exchange(ReadRequest(0), reply);
        // Correction: legacy appended any reply with size > 5.
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(h.transport.ScriptConsumed()); // no second page requested
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ReadPropagatesTransportErrorsAndCancellation)
{
    {
        Harness h;
        ScriptSession(h.transport);
        h.transport.ExpectWrite(ReadRequest(0));
        h.transport.QueueError(ErrorKind::kDisconnected, "unplugged");
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kDisconnected));
    }
    {
        Harness h;
        ScriptSession(h.transport);
        h.transport.ExpectWrite(ReadRequest(0));
        h.transport.QueueNoFrame();
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kTimeout));
    }
    {
        Harness h;
        ScriptSession(h.transport);
        h.transport.Exchange(ReadRequest(0), PageReply(0));
        h.transport.Exchange(ReadRequest(0x400), PageReply(0x400));
        // Cancel once the second page request is out: its reply is never read
        // and no third page is requested.
        h.cancellation.SetPredicate([&h] { return h.transport.WritesConsumed() >= kSessionWrites + 2; });
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kRead)), IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(h.transport.WritesConsumed(), kSessionWrites + 2);
    }
}

// ---- Write and TestWrite (Task 8) -------------------------------------

// get_changed_blocks():767-790 / check_romcrc():812-881 -- one CRC request per
// block, then the 200ms flush read after every compare.
void ScriptCompare(ScriptedKlineFlashTransport& t, std::string_view mcu, const bytes::Bytes& image,
                   const std::vector<unsigned>& differing)
{
    auto s = t.Section("compare");
    const FlashDevice *device = FindFlashDevice(mcu);
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        const FlashBlock& block = device->fblocks[i];
        std::uint32_t crc = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(block.start, block.len));
        if (std::ranges::find(differing, i) != differing.end())
        {
            crc ^= 0xFFFFFFFFU;
        }
        t.Exchange(CrcRequest(block), BeefReply(0x02, ComposeBe(crc)));
        t.QueueNoFrame(); // the 200ms flush read after every compare
    }
}
// init_flash_write():893-1022 -- MAX_MSG, MAX_BLK, then FLASH_ENABLE/DISABLE.
void ScriptInit(ScriptedKlineFlashTransport& t, std::uint8_t mode_opcode)
{
    auto s = t.Section("init_flash_write");
    t.Exchange(Beef(0x05), BeefReply(0x05, ComposeBe(std::uint32_t{0x00000204})));
    t.Exchange(Beef(0x06), BeefReply(0x06, ComposeBe(std::uint32_t{0x00001000})));
    t.Exchange(Beef(mode_opcode), BeefReply(mode_opcode, bytes::Bytes{0x00}));
}
// reflash_block():1068-1105 PROG_VOLT, then flash_block():1155-1357 BLANK_PAGE,
// 0x200-byte WRITE_FLASH_BUFFER chunks and a COMMIT/VALIDATE per 0x1000.
void ScriptReflash(ScriptedKlineFlashTransport& t, const bytes::Bytes& image, const FlashBlock& block,
                   std::uint8_t commit_opcode)
{
    auto s = t.Section("reflash_block");
    t.Exchange(Beef(0x04), BeefReply(0x04, bytes::Bytes{0x03, 0x84, 0x00, 0x00, 0x00})); // 900/50 = 18.0V
    t.Exchange(Beef(0x25, ComposeBe(block.start)), BeefReply(0x25));
    for (std::uint32_t address = block.start; address < block.start + block.len; address += 0x200)
    {
        t.Exchange(Beef(0x22, ComposeBe(address, bytes::ByteView(image).subspan(address, 0x200))),
                   BeefReply(0x22, bytes::Bytes{0x00}));
        if ((address + 0x200 - block.start) % 0x1000 == 0)
        {
            const std::uint32_t commit_start = address + 0x200 - 0x1000;
            const std::uint32_t crc = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(commit_start, 0x1000));
            t.Exchange(Beef(commit_opcode, ComposeBe(commit_start, std::uint16_t{0x1000}, crc)),
                       BeefReply(commit_opcode, bytes::Bytes{0x00}));
        }
    }
}

bytes::Bytes Sh7055Image()
{
    bytes::Bytes image(FindFlashDevice("SH7055")->romsize);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 7);
    }
    return image;
}

TEST(SubaruDensoSh705xKlineExecutor, WriteWithNoDifferencesFlashesNothing)
{
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    ScriptSession(h.transport);
    ScriptCompare(h.transport, "SH7055", image, {});
    const auto result = h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image));
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(h.transport.ScriptConsumed());
    EXPECT_THAT(
        h.events.logs,
        ::testing::Contains(std::pair<LogLevel, std::string>{
            LogLevel::kInfo, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***"}));
}

TEST(SubaruDensoSh705xKlineExecutor, WriteReflashesOnlyChangedBlocksWithCommit)
{
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    const FlashDevice *device = FindFlashDevice("SH7055");
    ScriptSession(h.transport);
    ScriptCompare(h.transport, "SH7055", image, {1, 8});
    ScriptInit(h.transport, 0x20);
    ScriptReflash(h.transport, image, device->fblocks[1], 0x24);
    ScriptReflash(h.transport, image, device->fblocks[8], 0x24);
    ScriptCompare(h.transport, "SH7055", image, {});

    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)), IsOk());
    EXPECT_TRUE(h.transport.ScriptConsumed());
    // The kernel upload reports first; the write then counts both blocks'
    // bytes (0x1000 + 0x8000) from zero.
    const auto write_start = std::ranges::find(h.events.progress_calls, std::pair{0, 0x9000});
    ASSERT_NE(write_start, h.events.progress_calls.end());
    EXPECT_EQ(std::distance(write_start, h.events.progress_calls.end()), 1 + 0x9000 / 0x200);
    EXPECT_EQ(h.events.progress_calls.back(), (std::pair{0x9000, 0x9000}));
}

TEST(SubaruDensoSh705xKlineExecutor, TestWriteUsesFlashDisableAndValidateNeverEnableOrCommit)
{
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    const FlashDevice *device = FindFlashDevice("SH7055");
    ScriptSession(h.transport);
    ScriptCompare(h.transport, "SH7055", image, {0});
    ScriptInit(h.transport, 0x21);
    ScriptReflash(h.transport, image, device->fblocks[0], 0x23);
    ScriptCompare(h.transport, "SH7055", image, {0}); // nothing committed, still differs

    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kTestWrite, "sub_ecu_denso_sh7055_04_cobb", "SH7055", image)), IsOk());
    EXPECT_TRUE(h.transport.ScriptConsumed());
    EXPECT_THAT(h.events.logs, ::testing::Contains(std::pair<LogLevel, std::string>{
                                   LogLevel::kInfo, "*** Test write PASS, it's ok to perform actual write! ***"}));
}

TEST(SubaruDensoSh705xKlineExecutor, BadFlashModeAckSendsNoEraseInEitherMode)
{
    for (const auto& [operation, opcode] : {std::pair{FlashOperation::kTestWrite, std::uint8_t{0x21}},
                                            std::pair{FlashOperation::kWrite, std::uint8_t{0x20}}})
    {
        SCOPED_TRACE(static_cast<int>(operation));
        Harness h;
        const bytes::Bytes image = Sh7055Image();
        ScriptSession(h.transport);
        ScriptCompare(h.transport, "SH7055", image, {0});
        h.transport.Exchange(Beef(0x05), BeefReply(0x05, ComposeBe(std::uint32_t{0x204})));
        h.transport.Exchange(Beef(0x06), BeefReply(0x06, ComposeBe(std::uint32_t{0x1000})));
        h.transport.Exchange(Beef(opcode), bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, opcode, 0x00});

        EXPECT_THAT(h.Run(MakePlan(operation, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(h.transport.ScriptConsumed()); // no PROG_VOLT, no BLANK_PAGE
    }
}

TEST(SubaruDensoSh705xKlineExecutor, WriteThatStillDiffersAfterReflashFails)
{
    // Correction: legacy logged "ERROR IN FLASH PROCESS" and returned success.
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    const FlashDevice *device = FindFlashDevice("SH7055");
    ScriptSession(h.transport);
    ScriptCompare(h.transport, "SH7055", image, {2});
    ScriptInit(h.transport, 0x20);
    ScriptReflash(h.transport, image, device->fblocks[2], 0x24);
    ScriptCompare(h.transport, "SH7055", image, {2});

    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(h.transport.ScriptConsumed());
    EXPECT_THAT(h.events.logs, ::testing::Contains(std::pair<LogLevel, std::string>{LogLevel::kError,
                                                                                    "*** ERROR IN FLASH PROCESS ***"}));
}

TEST(SubaruDensoSh705xKlineExecutor, ShortCrcReplyIsRejectedBeforeParsing)
{
    // Correction: legacy read at(5..8) after checking only `> 5`.
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    ScriptSession(h.transport);
    h.transport.Exchange(CrcRequest(FindFlashDevice("SH7055")->fblocks[0]),
                         bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x42, 0x12, 0x00});
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(h.transport.ScriptConsumed()); // no flush read, no second CRC request
}

TEST(SubaruDensoSh705xKlineExecutor, EachRejectedWriteStepStopsLaterCommands)
{
    // Reject PROG_VOLT, BLANK_PAGE, the first WRITE_FLASH_BUFFER, then COMMIT.
    const FlashDevice *device = FindFlashDevice("SH7055");
    const FlashBlock block = device->fblocks[0];
    const bytes::Bytes image = Sh7055Image();
    const std::uint32_t crc0 = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(0, 0x1000));
    const std::vector<std::vector<std::pair<bytes::Bytes, bytes::Bytes>>> prefixes{
        {{Beef(0x04), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00}}},
        {{Beef(0x04), BeefReply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0})},
         {Beef(0x25, ComposeBe(block.start)), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00}}},
        {{Beef(0x04), BeefReply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0})},
         {Beef(0x25, ComposeBe(block.start)), BeefReply(0x25)},
         {Beef(0x22, ComposeBe(std::uint32_t{0}, bytes::ByteView(image).subspan(0, 0x200))),
          bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, 0x22, 0x00}}},
    };
    for (const auto& prefix : prefixes)
    {
        SCOPED_TRACE(prefix.size());
        Harness h;
        ScriptSession(h.transport);
        ScriptCompare(h.transport, "SH7055", image, {0});
        ScriptInit(h.transport, 0x20);
        for (const auto& [request, reply] : prefix)
        {
            h.transport.Exchange(request, reply);
        }
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(h.transport.ScriptConsumed());
    }
    {
        Harness h;
        ScriptSession(h.transport);
        ScriptCompare(h.transport, "SH7055", image, {0});
        ScriptInit(h.transport, 0x20);
        h.transport.Exchange(Beef(0x04), BeefReply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0}));
        h.transport.Exchange(Beef(0x25, ComposeBe(block.start)), BeefReply(0x25));
        for (std::uint32_t address = 0; address < 0x1000; address += 0x200)
        {
            h.transport.Exchange(Beef(0x22, ComposeBe(address, bytes::ByteView(image).subspan(address, 0x200))),
                                 BeefReply(0x22, bytes::Bytes{0x00}));
        }
        h.transport.Exchange(Beef(0x24, ComposeBe(std::uint32_t{0}, std::uint16_t{0x1000}, crc0)),
                             bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, 0x24, 0x00});
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(h.transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xKlineExecutor, CompareUsesLegacyTimeoutsAndPacing)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    const bytes::Bytes image = Sh7055Image();
    ScriptProbeAlive(transport);
    ScriptCompare(transport, "SH7055", image, {});
    RecordingClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    ASSERT_THAT(executor.Execute(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image),
                                 transport, clock, cancellation, events),
                IsOk());
    // probe: 100 settle + 200 kernel-ID settle; then 16 x 5ms block pacing.
    std::vector<std::chrono::milliseconds> expected_sleeps{100ms, 200ms};
    expected_sleeps.insert(expected_sleeps.end(), 16, 5ms);
    EXPECT_EQ(clock.sleep_calls, expected_sleeps);
    std::vector<std::chrono::milliseconds> expected_reads{800ms};
    for (int i = 0; i < 16; ++i)
    {
        expected_reads.push_back(3000ms);
        expected_reads.push_back(200ms);
    }
    EXPECT_EQ(transport.read_timeouts, expected_reads);
}

TEST(SubaruDensoSh705xKlineExecutor, WriteStepsUseLegacyTimeoutsWithoutSettleDelays)
{
    // init_flash_write():905/947/1000 500ms; reflash_block():1080 500ms;
    // flash_block():1171/1231/1335 3000ms; the delay(500)/(50)/(200) there are
    // commented out in legacy and stay omitted.
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    const bytes::Bytes image = Sh7055Image();
    ScriptProbeAlive(transport);
    ScriptCompare(transport, "SH7055", image, {0});
    ScriptInit(transport, 0x20);
    ScriptReflash(transport, image, FindFlashDevice("SH7055")->fblocks[0], 0x24);
    ScriptCompare(transport, "SH7055", image, {});
    RecordingClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    ASSERT_THAT(executor.Execute(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image),
                                 transport, clock, cancellation, events),
                IsOk());
    std::vector<std::chrono::milliseconds> expected_sleeps{100ms, 200ms};
    expected_sleeps.insert(expected_sleeps.end(), 32, 5ms); // two compares, no write-path delays
    EXPECT_EQ(clock.sleep_calls, expected_sleeps);
    std::vector<std::chrono::milliseconds> compare_reads;
    for (int i = 0; i < 16; ++i)
    {
        compare_reads.push_back(3000ms);
        compare_reads.push_back(200ms);
    }
    std::vector<std::chrono::milliseconds> expected_reads{800ms};
    expected_reads.insert(expected_reads.end(), compare_reads.begin(), compare_reads.end());
    expected_reads.insert(expected_reads.end(), {500ms, 500ms, 500ms, 500ms, 3000ms});
    expected_reads.insert(expected_reads.end(), 9, 3000ms); // 8 chunks + 1 commit
    expected_reads.insert(expected_reads.end(), compare_reads.begin(), compare_reads.end());
    EXPECT_EQ(transport.read_timeouts, expected_reads);
}

TEST(SubaruDensoSh705xKlineExecutor, WriteLogsTheLegacyStrings)
{
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    ScriptProbeAlive(h.transport);
    ScriptCompare(h.transport, "SH7055", image, {0});
    ScriptInit(h.transport, 0x20);
    ScriptReflash(h.transport, image, FindFlashDevice("SH7055")->fblocks[0], 0x24);
    ScriptCompare(h.transport, "SH7055", image, {});
    ASSERT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)), IsOk());

    using L = LogLevel;
    using Entry = std::pair<LogLevel, std::string>;
    const std::uint32_t crc0 = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(0, 0x1000));
    const std::uint32_t crc1 = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(0x1000, 0x1000));
    // write_mem():661-662, get_changed_blocks():782, check_romcrc():864-878
    // for the first two blocks (block 0 differs).
    const std::vector<Entry> compare_head{
        {L::kInfo, "Writing ROM to Subaru 04 32-bit using K-Line"},
        {L::kInfo, "--- Comparing ECU flash memory pages to image file ---"},
        {L::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?"},
        {L::kInfo, "FB00\t0x00000000\t0x00001000"},
        {L::kDebug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", crc0 ^ 0xFFFFFFFFU, crc0)},
        {L::kInfo, std::format("\t{:08X}\t{:08X}", crc0 ^ 0xFFFFFFFFU, crc0)},
        {L::kInfo, "\tNO"},
        {L::kInfo, "FB01\t0x00001000\t0x00001000"},
        {L::kDebug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", crc1, crc1)},
        {L::kInfo, std::format("\t{:08X}\t{:08X}", crc1, crc1)},
        {L::kInfo, "\tYES"},
    };
    const auto head = std::ranges::search(h.events.logs, compare_head);
    EXPECT_FALSE(head.empty());

    // write_mem():671-680, 695; init_flash_write():893-1008;
    // reflash_block():1065-1089, 1115; flash_block():1148-1310; write_mem():709.
    std::vector<Entry> write_body{
        {L::kInfo, "Different blocks : "},
        {L::kInfo, "0, "},
        {L::kInfo, " (total: 1)"},
        {L::kInfo, "--- Start writing ROM file to ECU flash memory ---"},
        {L::kInfo, "Check max message length"},
        {L::kInfo, ": 0x0204"},
        {L::kInfo, "Check flashblock size"},
        {L::kInfo, ": 0x1000"},
        {L::kInfo, "Test write mode off, perform actual flash write"},
        {L::kError, "Flash mode succesfully set"}, // legacy emits this through LOG_E
        {L::kInfo, "Flash block addr: 0x00000000 len: 0x00001000"},
        {L::kInfo, "Check flash voltage"},
        {L::kInfo, ": 18V"},
        {L::kInfo, "Flash page erase addr: 0x00000000 len: 0x00001000"},
        {L::kInfo, "Erasing flash page..."},
        {L::kInfo, " erased"},
        {L::kInfo, "Start flash write addr: 0x00000000 len: 0x00001000"},
    };
    for (std::uint32_t address = 0; address < 0x1000; address += 0x200)
    {
        write_body.emplace_back(L::kDebug, "Data written to flash buffer");
        // FakeClock does not advance: 1ms per chunk, 0x200 * 1000 B/s, ~1 s.
        write_body.emplace_back(L::kInfo, std::format("Write flash buffer: 0x{:08X} ({}% - 512000 B/s, ~ 1 s remain)",
                                                      address, 100U * address / 0x1000U));
    }
    write_body.insert(write_body.end(),
                      {
                          {L::kInfo, "Flash buffer write complete... "},
                          {L::kDebug, std::format("Image CRC32: 0x{:x}", crc0)},
                          {L::kInfo, "Committ flash addr: 0x0"},
                          {L::kInfo, " len: 0x1000"},
                          {L::kInfo, std::format(" crc32: 0x{:x}", crc0)},
                          {L::kInfo, "Flash block ok"},
                          {L::kInfo, "Block 0 reflash complete."},
                          {L::kInfo, "--- Comparing ECU flash memory pages to image file after reflash ---"},
                      });
    const auto body = std::ranges::search(h.events.logs, write_body);
    EXPECT_FALSE(body.empty());
    EXPECT_EQ(h.events.logs.back(), (Entry{L::kInfo, " (total: 0)"}));
    EXPECT_THAT(h.events.notices, ::testing::ElementsAre("Writing ROM, please wait..."));
}

TEST(SubaruDensoSh705xKlineExecutor, FailedFlashBlockLogsTheLegacyRecoveryText)
{
    Harness h;
    const bytes::Bytes image = Sh7055Image();
    ScriptProbeAlive(h.transport);
    ScriptCompare(h.transport, "SH7055", image, {0});
    ScriptInit(h.transport, 0x20);
    h.transport.Exchange(Beef(0x04), BeefReply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0}));
    h.transport.Exchange(Beef(0x25, ComposeBe(std::uint32_t{0})), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::kBadResponse));
    ASSERT_GE(h.events.logs.size(), 3U);
    // flash_block():1180-1182, reflash_block():1109-1111, write_mem():703.
    EXPECT_EQ(h.events.logs[h.events.logs.size() - 3].first, LogLevel::kError);
    EXPECT_TRUE(h.events.logs[h.events.logs.size() - 3].second.starts_with("Wrong response from ECU: "));
    EXPECT_EQ(h.events.logs[h.events.logs.size() - 2],
              (std::pair<LogLevel, std::string>{LogLevel::kError,
                                                "Reflash error! Do not panic, do not reset the ECU immediately. The "
                                                "kernel is most likely still running and receiving commands!"}));
    EXPECT_EQ(h.events.logs.back(), (std::pair<LogLevel, std::string>{LogLevel::kInfo, "Block 0 reflash failed."}));
}

TEST(SubaruDensoSh705xKlineExecutor, CancellationDuringWriteStopsBeforeTheNextCommand)
{
    // Script: probe 1 + compare 16 + init 3 + PROG_VOLT 1 + BLANK_PAGE 1 +
    // 8 chunks + 1 commit + compare 16 = 47 writes. Cancelling once k are out
    // must send no further frame.
    for (std::size_t k = 1; k < 47; k += 3)
    {
        SCOPED_TRACE(k);
        Harness h;
        const bytes::Bytes image = Sh7055Image();
        ScriptProbeAlive(h.transport);
        ScriptCompare(h.transport, "SH7055", image, {0});
        ScriptInit(h.transport, 0x20);
        ScriptReflash(h.transport, image, FindFlashDevice("SH7055")->fblocks[0], 0x24);
        ScriptCompare(h.transport, "SH7055", image, {});
        h.cancellation.SetPredicate([&h, k] { return h.transport.WritesConsumed() >= k; });
        EXPECT_THAT(h.Run(MakePlan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(h.transport.WritesConsumed(), k);
    }
}

} // namespace
} // namespace fastecu::flash
