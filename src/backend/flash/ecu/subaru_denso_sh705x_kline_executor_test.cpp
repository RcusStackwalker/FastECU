#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::u24;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using namespace bytes::literals;
using namespace std::chrono_literals;

// ---- Wire transcription, independent of production helpers -------------

// Kernel frame: BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes beef(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return composeBeWithChecksum(bytes::sum8, std::uint16_t{0xBEEF}, std::uint16_t(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}
// A positive kernel reply: BE EF, length, opcode|0x40, data, sum8.
bytes::Bytes beef_reply(std::uint8_t opcode, bytes::ByteView data = {})
{
    return beef(static_cast<std::uint8_t>(opcode | 0x40), data);
}
// SSM request tester 0xF0 -> target 0x10: 80 10 F0 len payload sum8.
bytes::Bytes ssm(bytes::ByteView payload)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), payload);
}
// SSM reply target -> tester: 80 F0 10 len payload sum8.
bytes::Bytes ssm_reply(bytes::ByteView payload)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, 0xF0_b, 0x10_b, bytes::Byte(payload.size()), payload);
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

bytes::Bytes kernel_id_reply()
{
    return beef_reply(0x01, bytes::Bytes{'S', 'S', 'M', 'K'});
}

KernelImage kernel_for(std::string_view mcu)
{
    return KernelImage{.id = "k", .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U, .bytes = kKernelBytes};
}

FlashPlan make_plan(FlashOperation operation, std::string_view protocol = "sub_ecu_denso_sh7055_04",
                    std::string_view mcu = "SH7055", std::optional<bytes::Bytes> image = std::nullopt)
{
    if (operation != FlashOperation::Read && !image.has_value())
    {
        image = bytes::Bytes(find_flash_device(mcu)->romsize, 0xFF);
    }
    auto plan = build_subaru_denso_sh705x_kline_plan(operation, protocol, mcu, std::move(image), kernel_for(mcu));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// connect_bootloader():127-157 -- kernel probe at 62500.
void script_probe_dead(ScriptedKlineFlashTransport& t)
{
    auto s = t.section("probe");
    t.expectWrite(kKernelIdRequest);
    t.queue_no_frame();
}
void script_probe_alive(ScriptedKlineFlashTransport& t)
{
    auto s = t.section("probe");
    t.exchange(kKernelIdRequest, kernel_id_reply());
}

bytes::Bytes stock_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kKeyTable, SsmProtocol::kIndexTransformationStock);
}
bytes::Bytes ecutek_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kKeyTable, SsmProtocol::kIndexTransformationEcutek);
}
bytes::Bytes encrypted_kernel(bytes::ByteView balanced)
{
    return SsmProtocol::calculatePayload(balanced, static_cast<std::uint32_t>(balanced.size()), kEncryptTable,
                                         SsmProtocol::kIndexTransformationStock);
}

const bytes::Bytes kEcuIdPayload{0xFF, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44, 0x45};

// connect_bootloader():161-323 -- SSM handshake at 4800.
void script_handshake(ScriptedKlineFlashTransport& t, bytes::ByteView key)
{
    auto s = t.section("handshake");
    t.exchange(ssm(bytes::Bytes{0xBF}), ssm_reply(kEcuIdPayload));
    t.exchange(ssm(bytes::Bytes{0x81}), ssm_reply(bytes::Bytes{0xC1}));
    t.exchange(ssm(bytes::Bytes{0x83, 0x00}), ssm_reply(bytes::Bytes{0xC3}));
    t.exchange(ssm(bytes::Bytes{0x27, 0x01}), ssm_reply(composeBe(0x67_b, 0x01_b, kSeed)));
    t.exchange(ssm(composeBe(0x27_b, 0x02_b, key)), ssm_reply(bytes::Bytes{0x67, 0x02}));
    t.exchange(ssm(bytes::Bytes{0x10, 0x85, 0x02}), ssm_reply(bytes::Bytes{0x50}));
}

// upload_kernel():354-460 -- 34 / 36 / 31 at 15625.
void script_upload_frames(ScriptedKlineFlashTransport& t, std::uint32_t address)
{
    const bytes::Bytes encrypted = encrypted_kernel(kBalancedKernel);
    t.exchange(ssm(composeBe(0x34_b, u24(address), 0x04_b, u24(8))), ssm_reply(bytes::Bytes{0x74}));
    t.exchange(ssm(composeBe(0x36_b, u24(address), encrypted)), ssm_reply(bytes::Bytes{0x76}));
    t.exchange(ssm(bytes::Bytes{0x31, 0x01, 0x01}), ssm_reply(bytes::Bytes{0x71}));
}

// upload_kernel():354-496 -- 34 / 36 / 31 at 15625, then kernel ID at 62500.
void script_upload(ScriptedKlineFlashTransport& t, std::uint32_t address)
{
    auto s = t.section("upload");
    script_upload_frames(t, address);
    t.exchange(kKernelIdRequest, kernel_id_reply());
}

void script_session(ScriptedKlineFlashTransport& t, bytes::ByteView key = {}, std::uint32_t address = 0xFFFF6004)
{
    script_probe_dead(t);
    script_handshake(t, key.empty() ? stock_key(kSeed) : bytes::Bytes(key.begin(), key.end()));
    script_upload(t, address);
}

struct Harness
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    Result<FlashExecutionResult> run(const FlashPlan& plan)
    {
        return executor.execute(plan, transport, clock, cancellation, events);
    }
};

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelMatchesHandWorkedVectors)
{
    // AA BB CC DD +00 00 -> pad to 8 -> drop 2 -> AA BB CC DD 00 00.
    // Words (bytes past the end read as zero): AABBCCDD, 00000000.
    // u16 sum = 0xCCDD; 0x5AA5 - 0xCCDD = 0x8DC8 (mod 2^16).
    EXPECT_EQ(denso_sh705x_kline_balanced_kernel(kKernelBytes), kBalancedKernel);
    // 01 02 03 +00 00 -> 01 02 03 00 00 00 00 00 -> drop 2 -> 6 bytes.
    // u16 sum = 0x0300; balance = 0x57A5.
    EXPECT_EQ(denso_sh705x_kline_balanced_kernel(bytes::Bytes{0x01, 0x02, 0x03}),
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
        const bytes::Bytes out = denso_sh705x_kline_balanced_kernel(kernel);
        ASSERT_EQ(out.size() % 4, 0U);
        std::uint16_t sum = 0;
        for (std::size_t i = 0; i < out.size(); i += 4)
        {
            sum = static_cast<std::uint16_t>(sum + bytes::readU32Be(out, i));
        }
        EXPECT_EQ(sum, 0x5AA5);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, TransportSetupIsNonIso14230At4800)
{
    SubaruDensoSh705xKlineExecutor executor;
    const auto config = executor.transport_setup(make_plan(FlashOperation::Read));
    ASSERT_THAT(config, IsOk());
    EXPECT_EQ(config->baud, 4800);
    EXPECT_FALSE(config->iso14230);
    EXPECT_EQ(config->tester_id, 0xF0);
    EXPECT_EQ(config->target_id, 0x10);
    EXPECT_EQ(config->parity, KlineParity::None);
}

TEST(SubaruDensoSh705xKlineExecutor, BoundAttemptResetsBeforeConfigure)
{
    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    auto *observed = transport.get();
    observed->set_baud_result_ = fail(ErrorKind::Disconnected, "stop after lifecycle");
    auto attempt = bind_flash_attempt(make_plan(FlashOperation::Read),
                                      std::make_unique<SubaruDensoSh705xKlineExecutor>(), std::move(transport));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    // The first setBaud (connect_bootloader():127, 62500) fails and stops execute().
    EXPECT_THAT(attempt->run(clock, cancellation, events), IsErr(ErrorKind::Disconnected));
    // execute():67 reset_connection() precedes every setter and open_serial_port().
    EXPECT_THAT(observed->lifecycle_calls_, ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(SubaruDensoSh705xKlineExecutor, CancellationAroundResetStopsBeforeConfigure)
{
    for (const std::size_t check : {1U, 2U})
    {
        SCOPED_TRACE(check);
        ScriptedKlineFlashTransport transport;
        FakeClock clock;
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(check);
        SubaruDensoSh705xKlineExecutor executor;

        EXPECT_THAT(executor.before_transport_configure(transport, clock, cancellation), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(transport.reset_call_count_, check == 1 ? 0 : 1);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ResetFailurePropagates)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result_ = fail(ErrorKind::Disconnected, "no adapter");
    FakeClock clock;
    FakeCancellationToken cancellation;
    SubaruDensoSh705xKlineExecutor executor;

    EXPECT_THAT(executor.before_transport_configure(transport, clock, cancellation), IsErr(ErrorKind::Disconnected));
}

TEST(SubaruDensoSh705xKlineExecutor, RejectsAForeignPlanBeforeIo)
{
    Harness h;
    FlashPlanFields fields{.operation = FlashOperation::Read,
                           .family = FlashFamily::SubaruUnisiaJecs,
                           .transport = TransportKind::Kline,
                           .target_id = "sub_ecu_unisia_jecs_m3779x",
                           .mcu_name = "M3779x",
                           .transfer_region = {0, 0x10000},
                           .erase_regions = {},
                           .image = std::nullopt,
                           .kernel = std::nullopt,
                           .family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true},
                           .confirmations = {}};
    auto foreign = validate_and_build(std::move(fields));
    ASSERT_THAT(foreign, IsOk());

    EXPECT_THAT(h.executor.transport_setup(*foreign), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(h.run(*foreign), IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(h.transport.writesConsumed(), 0U);
}

// ---- Session: probe, handshake, upload (Task 6) -------------------------
// The operation tail is still a stub; these pin the transcript, not its kind.

TEST(SubaruDensoSh705xKlineExecutor, FullSessionIsByteExactWithLegacyBaudsAndTimeouts)
{
    Harness h;
    script_session(h.transport);

    const auto result = h.run(make_plan(FlashOperation::Read));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.transport.baud_calls_, ::testing::ElementsAre(62500, 4800, 15625, 62500));
    // probe 800, BF..10 six x 2000, 34 3000, 36 2000, 31 3000, kernel ID 800.
    EXPECT_THAT(h.transport.read_timeouts_, ::testing::ElementsAre(800ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms,
                                                                   2000ms, 3000ms, 2000ms, 3000ms, 800ms));
    // delay(100) after 62500, delay(200) in request_kernel_id(), delay(100)
    // after 4800, delay(100) after 31, delay(200) in request_kernel_id().
    EXPECT_EQ(h.clock.elapsed(), 700ms);
}

TEST(SubaruDensoSh705xKlineExecutor, SessionLogsTheLegacyStrings)
{
    // TestWrite: this test pins the session transcript, not the operation
    // kind. Task 7 wires FlashOperation::Read past the session into
    // read_mem(), which would add its own notice/log/progress entries here.
    Harness h;
    script_session(h.transport);
    ASSERT_FALSE(h.run(make_plan(FlashOperation::TestWrite)).has_value());

    using L = LogLevel;
    const std::vector<std::pair<LogLevel, std::string>> expected{
        {L::Info, "Connecting to Subaru 04 32-bit K-Line bootloader, please wait..."},
        {L::Info, "Checking if kernel is already running..."},
        {L::Info, "Requesting kernel ID"},
        {L::Error, "No valid response from ECU"},
        {L::Info, "No response from kernel, initialising ECU..."},
        {L::Info, "Requesting ECU ID"},
        {L::Info, "ECU ID: 4142434445"},
        {L::Info, "Requesting to start communication"},
        {L::Info, "Start communication ok"},
        {L::Info, "Requesting timings params"},
        {L::Info, "Timing parameters ok"},
        {L::Info, "Requesting seed"},
        {L::Info, "Seed request ok"},
        {L::Info, "Received seed: 11 22 33 44 "},
        {L::Info, "Calculated seed key: " + bytes::toHex(stock_key(kSeed))},
        {L::Info, "Sending seed key to ECU"},
        {L::Info, "Seed key ok"},
        {L::Info, "Set session mode"},
        {L::Info, "Succesfully set to programming session"},
        {L::Info, "Initializing Subaru 04 32-bit K-Line kernel upload, please wait..."},
        {L::Debug, "Start address to upload kernel: 0xffff6004"},
        {L::Info, "Requesting kernel upload"},
        {L::Info, "Kernel upload request ok"},
        {L::Info, "Transfer kernel data"},
        {L::Info, "Kernel uploaded"},
        {L::Info, "Jump to kernel"},
        {L::Info, "Kernel started, initializing..."},
        {L::Info, "Requesting kernel ID"},
        {L::Info, "Kernel ID: SSMK"},
    };
    EXPECT_EQ(h.events.logs, expected);
    EXPECT_THAT(h.events.notices, ::testing::ElementsAre("Preparing, please wait..."));
}

TEST(SubaruDensoSh705xKlineExecutor, EcutekProtocolSendsTheEcutekKey)
{
    Harness h;
    script_session(h.transport, ecutek_key(kSeed));
    ASSERT_NE(ecutek_key(kSeed), stock_key(kSeed));
    EXPECT_FALSE(h.run(make_plan(FlashOperation::Read, "sub_ecu_denso_sh7055_04_ecutek")).has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, Sh7058UploadsToItsOwnKernelAddress)
{
    Harness h;
    script_session(h.transport, {}, 0xFFFF3000);
    EXPECT_FALSE(h.run(make_plan(FlashOperation::Read, "sub_ecu_denso_sh7058", "SH7058")).has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, KernelTransferSplitsInto0x80ByteBlocks)
{
    // send_sid_36_transferdata():1533-1566 -- 0x80-byte blocks at addr +
    // blockno * 0x80; the last block carries the remainder.
    const bytes::Bytes kernel(0x100, 0x5A);
    const bytes::Bytes balanced = denso_sh705x_kline_balanced_kernel(kernel);
    ASSERT_EQ(balanced.size(), 0x104U);
    const bytes::Bytes encrypted = encrypted_kernel(balanced);
    const bytes::ByteView view(encrypted);

    Harness h;
    script_probe_dead(h.transport);
    script_handshake(h.transport, stock_key(kSeed));
    {
        auto s = h.transport.section("upload");
        h.transport.exchange(ssm(composeBe(0x34_b, u24(0xFFFF6004), 0x04_b, u24(0x104))),
                             ssm_reply(bytes::Bytes{0x74}));
        h.transport.exchange(ssm(composeBe(0x36_b, u24(0xFFFF6004), view.subspan(0, 0x80))),
                             ssm_reply(bytes::Bytes{0x76}));
        h.transport.exchange(ssm(composeBe(0x36_b, u24(0xFFFF6084), view.subspan(0x80, 0x80))),
                             ssm_reply(bytes::Bytes{0x76}));
        h.transport.exchange(ssm(composeBe(0x36_b, u24(0xFFFF6104), view.subspan(0x100, 4))),
                             ssm_reply(bytes::Bytes{0x76}));
        h.transport.exchange(ssm(bytes::Bytes{0x31, 0x01, 0x01}), ssm_reply(bytes::Bytes{0x71}));
        h.transport.exchange(kKernelIdRequest, kernel_id_reply());
    }
    // TestWrite: this test pins the kernel-transfer chunking, not the
    // operation kind. FlashOperation::Read would continue past the session
    // into read_mem(), adding its own progress calls to progress_calls.
    auto plan =
        build_subaru_denso_sh705x_kline_plan(FlashOperation::TestWrite, "sub_ecu_denso_sh7055_04", "SH7055",
                                             bytes::Bytes(find_flash_device("SH7055")->romsize, 0xFF),
                                             KernelImage{.id = "k", .load_address = 0xFFFF6004U, .bytes = kernel});
    ASSERT_THAT(plan, IsOk());
    EXPECT_FALSE(h.run(*plan).has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.events.progress_calls, ::testing::ElementsAre(std::pair{0, 0x104}, std::pair{0x80, 0x104},
                                                                std::pair{0x100, 0x104}, std::pair{0x104, 0x104}));
}

TEST(SubaruDensoSh705xKlineExecutor, RejectedKernelTransferBlockStops)
{
    Harness h;
    script_probe_dead(h.transport);
    script_handshake(h.transport, stock_key(kSeed));
    h.transport.exchange(ssm(composeBe(0x34_b, u24(0xFFFF6004), 0x04_b, u24(8))), ssm_reply(bytes::Bytes{0x74}));
    h.transport.exchange(ssm(composeBe(0x36_b, u24(0xFFFF6004), encrypted_kernel(kBalancedKernel))),
                         ssm_reply(bytes::Bytes{0x7F, 0x36, 0x22}));
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, LiveKernelSkipsHandshakeAndUpload)
{
    // TestWrite: pins that a live kernel skips the handshake/upload notices,
    // not the operation kind -- FlashOperation::Read now emits its own
    // "Reading ROM, please wait..." notice once the session succeeds.
    Harness h;
    script_probe_alive(h.transport);
    EXPECT_FALSE(h.run(make_plan(FlashOperation::TestWrite)).has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.transport.baud_calls_, ::testing::ElementsAre(62500));
    EXPECT_TRUE(h.events.notices.empty());
}

TEST(SubaruDensoSh705xKlineExecutor, MinimalLiveKernelReplyHasAnEmptyId)
{
    // A 5-byte reply passes legacy's `> 4` check; remove(0, 5) leaves nothing.
    // TestWrite: this test pins the probe's own logging, not the operation
    // kind -- FlashOperation::Read now logs and reads on past the session.
    Harness h;
    {
        auto s = h.transport.section("probe");
        h.transport.exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x41});
    }
    EXPECT_FALSE(h.run(make_plan(FlashOperation::TestWrite)).has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
    ASSERT_FALSE(h.events.logs.empty());
    EXPECT_EQ(h.events.logs.back(), (std::pair<LogLevel, std::string>{LogLevel::Info, "Kernel ID: "}));
}

TEST(SubaruDensoSh705xKlineExecutor, WrongProbeReplyFallsThroughToHandshake)
{
    Harness h;
    {
        auto s = h.transport.section("probe");
        h.transport.exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    }
    script_handshake(h.transport, stock_key(kSeed));
    script_upload(h.transport, 0xFFFF6004);
    EXPECT_FALSE(h.run(make_plan(FlashOperation::Read)).has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, ShortEcuIdReplyIsRejectedBeforeSlicing)
{
    // Correction: legacy removed 8 bytes after checking only `> 4`.
    Harness h;
    script_probe_dead(h.transport);
    h.transport.exchange(ssm(bytes::Bytes{0xBF}), ssm_reply(bytes::Bytes{0xFF, 0x00}));
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed());
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
    const std::vector<bytes::Bytes> requests{ssm(bytes::Bytes{0xBF}),
                                             ssm(bytes::Bytes{0x81}),
                                             ssm(bytes::Bytes{0x83, 0x00}),
                                             ssm(bytes::Bytes{0x27, 0x01}),
                                             ssm(composeBe(0x27_b, 0x02_b, stock_key(kSeed))),
                                             ssm(bytes::Bytes{0x10, 0x85, 0x02})};
    const std::vector<bytes::Bytes> good{ssm_reply(kEcuIdPayload),
                                         ssm_reply(bytes::Bytes{0xC1}),
                                         ssm_reply(bytes::Bytes{0xC3}),
                                         ssm_reply(composeBe(0x67_b, 0x01_b, kSeed)),
                                         ssm_reply(bytes::Bytes{0x67, 0x02}),
                                         ssm_reply(bytes::Bytes{0x50})};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.step);
        Harness h;
        script_probe_dead(h.transport);
        for (int i = 0; i < c.step; ++i)
        {
            h.transport.exchange(requests[i], good[i]);
        }
        h.transport.exchange(requests[c.step], ssm_reply(c.reply_payload));
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed()); // nothing sent after the bad reply
    }
}

TEST(SubaruDensoSh705xKlineExecutor, NegativeHandshakeReplyLogsTheNrc)
{
    // connect_bootloader():170 -- "Wrong response from ECU: " + parse_nrc_message(mid(4)).
    Harness h;
    script_probe_dead(h.transport);
    h.transport.exchange(ssm(bytes::Bytes{0xBF}), ssm_reply(bytes::Bytes{0x7F, 0xBF, 0x12}));
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
    ASSERT_FALSE(h.events.logs.empty());
    EXPECT_EQ(h.events.logs.back().first, LogLevel::Error);
    EXPECT_TRUE(h.events.logs.back().second.starts_with("Wrong response from ECU: "));
    EXPECT_NE(h.events.logs.back().second, "Wrong response from ECU: Not a valid answer");
}

TEST(SubaruDensoSh705xKlineExecutor, SilentHandshakeStepIsATimeout)
{
    Harness h;
    script_probe_dead(h.transport);
    h.transport.expectWrite(ssm(bytes::Bytes{0xBF}));
    h.transport.queue_no_frame();
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Timeout));
}

TEST(SubaruDensoSh705xKlineExecutor, FailedUploadBaudChangeStops)
{
    // setBaud results are shared across calls; fail from the third call on.
    struct FailThirdBaud final : ScriptedKlineFlashTransport
    {
        using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;
        Status setBaud(int baud) override
        {
            baud_calls_.push_back(baud);
            return baud_calls_.size() >= 3 ? fail(ErrorKind::Disconnected, "baud") : Status{};
        }
    };
    FailThirdBaud t{ScriptedTransportInitialState::Open};
    script_probe_dead(t);
    script_handshake(t, stock_key(kSeed));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;
    EXPECT_THAT(executor.execute(make_plan(FlashOperation::Read), t, clock, cancellation, events),
                IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(t.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, FinalBaudChangeFailureIsNowChecked)
{
    // Correction: legacy ignored upload_kernel()'s change_port_speed("62500").
    struct FailFourthBaud final : ScriptedKlineFlashTransport
    {
        using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;
        Status setBaud(int baud) override
        {
            baud_calls_.push_back(baud);
            return baud_calls_.size() == 4 ? fail(ErrorKind::Disconnected, "baud") : Status{};
        }
    };
    FailFourthBaud t{ScriptedTransportInitialState::Open};
    script_probe_dead(t);
    script_handshake(t, stock_key(kSeed));
    {
        auto s = t.section("upload without kernel ID");
        script_upload_frames(t, 0xFFFF6004);
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;
    EXPECT_THAT(executor.execute(make_plan(FlashOperation::Read), t, clock, cancellation, events),
                IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(t.scriptConsumed()); // no kernel-ID request after the failed baud change
}

TEST(SubaruDensoSh705xKlineExecutor, DeadKernelAfterUploadIsABadResponse)
{
    Harness h;
    script_probe_dead(h.transport);
    script_handshake(h.transport, stock_key(kSeed));
    {
        auto s = h.transport.section("upload");
        script_upload_frames(h.transport, 0xFFFF6004);
        h.transport.exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    }
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, SilentKernelAfterUploadIsATimeout)
{
    Harness h;
    script_probe_dead(h.transport);
    script_handshake(h.transport, stock_key(kSeed));
    {
        auto s = h.transport.section("upload");
        script_upload_frames(h.transport, 0xFFFF6004);
        h.transport.expectWrite(kKernelIdRequest);
        h.transport.queue_no_frame();
    }
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(h.transport.scriptConsumed());
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
        script_session(h.transport);
        h.cancellation.set_predicate([&h, k] { return h.transport.writesConsumed() >= k; });
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(h.transport.writesConsumed(), k);
    }
}

// ---- Read (Task 7) -------------------------------------------------------

// read_mem(): READ_AREA [00, addr24, 0x0400], reply BE EF len 43 <0x400 bytes> sum8.
bytes::Bytes read_request(std::uint32_t address)
{
    return beef(0x03, composeBe(0x00_b, u24(address), std::uint16_t{0x0400}));
}
bytes::Bytes page_reply(std::uint32_t address)
{
    bytes::Bytes data(0x400);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<bytes::Byte>((address >> 10U) + i);
    }
    return beef_reply(0x03, data);
}

TEST(SubaruDensoSh705xKlineExecutor, ReadReturnsTheWholeRomAndTheRomId)
{
    Harness h;
    script_session(h.transport);
    const std::uint32_t romsize = find_flash_device("SH7055")->romsize;
    bytes::Bytes expected;
    for (std::uint32_t address = 0; address < romsize; address += 0x400)
    {
        h.transport.exchange(read_request(address), page_reply(address));
        const bytes::Bytes reply = page_reply(address);
        expected.insert(expected.end(), reply.begin() + 5, reply.end() - 1);
    }

    const auto result = h.run(make_plan(FlashOperation::Read));

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_EQ(result->read_bytes, expected);
    // execute()/connect_bootloader():206 RomId = ecuid + "_".
    EXPECT_EQ(result->rom_id, std::optional<std::string>("4142434445_"));
    // execute():92-93 -- externalLoggerMessage() then LOG_I() before read_mem().
    EXPECT_THAT(h.events.notices, ::testing::Contains("Reading ROM, please wait..."));
}

TEST(SubaruDensoSh705xKlineExecutor, ReadWithLiveKernelHasNoRomId)
{
    Harness h;
    script_probe_alive(h.transport);
    const std::uint32_t romsize = find_flash_device("SH7055")->romsize;
    for (std::uint32_t address = 0; address < romsize; address += 0x400)
    {
        h.transport.exchange(read_request(address), page_reply(address));
    }
    const auto result = h.run(make_plan(FlashOperation::Read));
    ASSERT_THAT(result, IsOk());
    EXPECT_FALSE(result->rom_id.has_value());
}

TEST(SubaruDensoSh705xKlineExecutor, ReadRejectsShortBadChecksumAndWrongOpcodePages)
{
    bytes::Bytes short_page = page_reply(0);
    short_page.erase(short_page.begin() + 10); // one data byte missing
    bytes::Bytes bad_sum = page_reply(0);
    bad_sum.back() ^= 0x01U;
    bytes::Bytes wrong_op = page_reply(0);
    wrong_op[4] = 0x7F;
    for (const bytes::Bytes& reply : {short_page, bad_sum, wrong_op})
    {
        Harness h;
        script_session(h.transport);
        h.transport.exchange(read_request(0), reply);
        // Correction: legacy appended any reply with size > 5.
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed()); // no second page requested
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ReadPropagatesTransportErrorsAndCancellation)
{
    {
        Harness h;
        script_session(h.transport);
        h.transport.expectWrite(read_request(0));
        h.transport.queue_error(ErrorKind::Disconnected, "unplugged");
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Disconnected));
    }
    {
        Harness h;
        script_session(h.transport);
        h.transport.expectWrite(read_request(0));
        h.transport.queue_no_frame();
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Timeout));
    }
    {
        Harness h;
        script_session(h.transport);
        h.transport.exchange(read_request(0), page_reply(0));
        h.transport.exchange(read_request(0x400), page_reply(0x400));
        // Cancel once the second page request is out: its reply is never read
        // and no third page is requested.
        h.cancellation.set_predicate([&h] { return h.transport.writesConsumed() >= kSessionWrites + 2; });
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(h.transport.writesConsumed(), kSessionWrites + 2);
    }
}

} // namespace
} // namespace fastecu::flash
