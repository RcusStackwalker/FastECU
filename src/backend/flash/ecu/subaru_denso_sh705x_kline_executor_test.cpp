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
    return beef(static_cast<std::uint8_t>(opcode | 0x40U), data);
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

// check_romcrc():812-831 -- CRC [addr32, 00, len24] -> BE EF len 42 crc32 sum8.
bytes::Bytes crc_request(const flashblock& block)
{
    return beef(0x02, composeBe(block.start, 0x00_b, u24(block.len)));
}

// The session tests below pin only the session; the Write/TestWrite tail's
// first frame is the block-0 CRC request, and a transport error on its reply
// stops the tail deterministically before any further frame.
void script_stop_at_first_crc(ScriptedKlineFlashTransport& t, std::string_view mcu = "SH7055")
{
    auto s = t.section("stop at first CRC");
    t.expectWrite(crc_request(find_flash_device(mcu)->fblocks[0]));
    t.queue_error(ErrorKind::Disconnected, "stop after session");
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
    // TestWrite, stopped at the tail's first frame (the block-0 CRC request).
    Harness h;
    script_session(h.transport);
    script_stop_at_first_crc(h.transport);

    ASSERT_THAT(h.run(make_plan(FlashOperation::TestWrite)), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(h.transport.scriptConsumed());
    // execute():68 -- the header flag is cleared once, before any frame.
    EXPECT_EQ(h.transport.header_mode_calls_, std::vector<bool>{false});
    EXPECT_THAT(h.transport.baud_calls_, ::testing::ElementsAre(62500, 4800, 15625, 62500));
    // probe 800, BF..10 six x 2000, 34 3000, 36 2000, 31 3000, kernel ID 800,
    // then the tail's block-0 CRC read at 3000 that stops the run.
    EXPECT_THAT(h.transport.read_timeouts_, ::testing::ElementsAre(800ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms,
                                                                   2000ms, 3000ms, 2000ms, 3000ms, 800ms, 3000ms));
    // delay(100) after 62500, delay(200) in request_kernel_id(), delay(100)
    // after 4800, delay(100) after 31, delay(200) in request_kernel_id().
    EXPECT_EQ(h.clock.elapsed(), 700ms);
}

TEST(SubaruDensoSh705xKlineExecutor, HeaderFlagFailurePropagatesBeforeAnyWrite)
{
    // execute():68 -- set_add_iso14230_header(false) precedes the first setBaud and write.
    Harness h;
    h.transport.set_add_iso14230_header_result_ = fail(ErrorKind::Disconnected, "header flag");

    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(h.transport.header_mode_calls_, std::vector<bool>{false});
    EXPECT_TRUE(h.transport.baud_calls_.empty());
    EXPECT_EQ(h.transport.writesConsumed(), 0U);
}

TEST(SubaruDensoSh705xKlineExecutor, SessionLogsTheLegacyStrings)
{
    // TestWrite: this test pins the session transcript, not the operation
    // kind. The tail is stopped at its first frame (the block-0 CRC request)
    // and only the session's prefix of the log is compared.
    Harness h;
    script_session(h.transport);
    script_stop_at_first_crc(h.transport);
    ASSERT_THAT(h.run(make_plan(FlashOperation::TestWrite)), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(h.transport.scriptConsumed());

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
    ASSERT_GE(h.events.logs.size(), expected.size());
    EXPECT_EQ(std::vector(h.events.logs.begin(), h.events.logs.begin() + static_cast<std::ptrdiff_t>(expected.size())),
              expected);
    // execute():84 during the session, then :98 once the write tail starts.
    EXPECT_THAT(h.events.notices, ::testing::ElementsAre("Preparing, please wait...", "Writing ROM, please wait..."));
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
    script_stop_at_first_crc(h.transport);
    // TestWrite: this test pins the kernel-transfer chunking, not the
    // operation kind. The tail stops at its first CRC request, before
    // write_mem() reports any progress of its own.
    auto plan =
        build_subaru_denso_sh705x_kline_plan(FlashOperation::TestWrite, "sub_ecu_denso_sh7055_04", "SH7055",
                                             bytes::Bytes(find_flash_device("SH7055")->romsize, 0xFF),
                                             KernelImage{.id = "k", .load_address = 0xFFFF6004U, .bytes = kernel});
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(h.run(*plan), IsErr(ErrorKind::Disconnected));
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
    // TestWrite: pins that a live kernel skips the handshake/upload, not the
    // operation kind. The tail stops at its first CRC request; its own
    // "Writing ROM, please wait..." notice is the only one, so the session's
    // "Preparing, please wait..." is absent.
    Harness h;
    script_probe_alive(h.transport);
    script_stop_at_first_crc(h.transport);
    EXPECT_THAT(h.run(make_plan(FlashOperation::TestWrite)), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.transport.baud_calls_, ::testing::ElementsAre(62500));
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
        auto s = h.transport.section("probe");
        h.transport.exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x41});
    }
    script_stop_at_first_crc(h.transport);
    EXPECT_THAT(h.run(make_plan(FlashOperation::TestWrite)), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(h.transport.scriptConsumed());
    const std::pair<LogLevel, std::string> tail_start{LogLevel::Info, "Writing ROM to Subaru 04 32-bit using K-Line"};
    const auto tail = std::ranges::find(h.events.logs, tail_start);
    ASSERT_NE(tail, h.events.logs.begin());
    ASSERT_NE(tail, h.events.logs.end());
    EXPECT_EQ(*std::prev(tail), (std::pair<LogLevel, std::string>{LogLevel::Info, "Kernel ID: "}));
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
    // read_mem():523 and :628 bracket the page loop.
    using L = LogLevel;
    const auto& logs = h.events.logs;
    const auto start = std::ranges::find(
        logs, std::pair<LogLevel, std::string>{L::Info, "Reading ROM from Subaru 04 32-bit using K-Line"});
    ASSERT_NE(start, logs.end());
    EXPECT_THAT(std::vector(start, logs.end()),
                ::testing::ElementsAre(::testing::Pair(L::Info, "Reading ROM from Subaru 04 32-bit using K-Line"),
                                       ::testing::Pair(L::Info, "Start reading ROM, please wait..."),
                                       ::testing::Pair(L::Info, "ROM read ready")));
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

// ---- Write and TestWrite (Task 8) -------------------------------------

// get_changed_blocks():767-790 / check_romcrc():812-881 -- one CRC request per
// block, then the 200ms flush read after every compare.
void script_compare(ScriptedKlineFlashTransport& t, std::string_view mcu, const bytes::Bytes& image,
                    const std::vector<unsigned>& differing)
{
    auto s = t.section("compare");
    const flashdev_t *device = find_flash_device(mcu);
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        const flashblock& block = device->fblocks[i];
        std::uint32_t crc = fastecu::checksum::crc32(bytes::ByteView(image).subspan(block.start, block.len));
        if (std::ranges::find(differing, i) != differing.end())
        {
            crc ^= 0xFFFFFFFFU;
        }
        t.exchange(crc_request(block), beef_reply(0x02, composeBe(crc)));
        t.queue_no_frame(); // the 200ms flush read after every compare
    }
}
// init_flash_write():893-1022 -- MAX_MSG, MAX_BLK, then FLASH_ENABLE/DISABLE.
void script_init(ScriptedKlineFlashTransport& t, std::uint8_t mode_opcode)
{
    auto s = t.section("init_flash_write");
    t.exchange(beef(0x05), beef_reply(0x05, composeBe(std::uint32_t{0x00000204})));
    t.exchange(beef(0x06), beef_reply(0x06, composeBe(std::uint32_t{0x00001000})));
    t.exchange(beef(mode_opcode), beef_reply(mode_opcode, bytes::Bytes{0x00}));
}
// reflash_block():1068-1105 PROG_VOLT, then flash_block():1155-1357 BLANK_PAGE,
// 0x200-byte WRITE_FLASH_BUFFER chunks and a COMMIT/VALIDATE per 0x1000.
void script_reflash(ScriptedKlineFlashTransport& t, const bytes::Bytes& image, const flashblock& block,
                    std::uint8_t commit_opcode)
{
    auto s = t.section("reflash_block");
    t.exchange(beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0x00, 0x00, 0x00})); // 900/50 = 18.0V
    t.exchange(beef(0x25, composeBe(block.start)), beef_reply(0x25));
    for (std::uint32_t address = block.start; address < block.start + block.len; address += 0x200)
    {
        t.exchange(beef(0x22, composeBe(address, bytes::ByteView(image).subspan(address, 0x200))),
                   beef_reply(0x22, bytes::Bytes{0x00}));
        if ((address + 0x200 - block.start) % 0x1000 == 0)
        {
            const std::uint32_t commit_start = address + 0x200 - 0x1000;
            const std::uint32_t crc = fastecu::checksum::crc32(bytes::ByteView(image).subspan(commit_start, 0x1000));
            t.exchange(beef(commit_opcode, composeBe(commit_start, std::uint16_t{0x1000}, crc)),
                       beef_reply(commit_opcode, bytes::Bytes{0x00}));
        }
    }
}

bytes::Bytes sh7055_image()
{
    bytes::Bytes image(find_flash_device("SH7055")->romsize);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 7);
    }
    return image;
}

TEST(SubaruDensoSh705xKlineExecutor, WriteWithNoDifferencesFlashesNothing)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {});
    const auto result = h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image));
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(
        h.events.logs,
        ::testing::Contains(std::pair<LogLevel, std::string>{
            LogLevel::Info, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***"}));
}

TEST(SubaruDensoSh705xKlineExecutor, WriteReflashesOnlyChangedBlocksWithCommit)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    const flashdev_t *device = find_flash_device("SH7055");
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {1, 8});
    script_init(h.transport, 0x20);
    script_reflash(h.transport, image, device->fblocks[1], 0x24);
    script_reflash(h.transport, image, device->fblocks[8], 0x24);
    script_compare(h.transport, "SH7055", image, {});

    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)), IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
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
    const bytes::Bytes image = sh7055_image();
    const flashdev_t *device = find_flash_device("SH7055");
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {0});
    script_init(h.transport, 0x21);
    script_reflash(h.transport, image, device->fblocks[0], 0x23);
    script_compare(h.transport, "SH7055", image, {0}); // nothing committed, still differs

    EXPECT_THAT(h.run(make_plan(FlashOperation::TestWrite, "sub_ecu_denso_sh7055_04_cobb", "SH7055", image)), IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.events.logs, ::testing::Contains(std::pair<LogLevel, std::string>{
                                   LogLevel::Info, "*** Test write PASS, it's ok to perform actual write! ***"}));
}

TEST(SubaruDensoSh705xKlineExecutor, BadFlashModeAckSendsNoEraseInEitherMode)
{
    for (const auto& [operation, opcode] : {std::pair{FlashOperation::TestWrite, std::uint8_t{0x21}},
                                            std::pair{FlashOperation::Write, std::uint8_t{0x20}}})
    {
        SCOPED_TRACE(static_cast<int>(operation));
        Harness h;
        const bytes::Bytes image = sh7055_image();
        script_session(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        h.transport.exchange(beef(0x05), beef_reply(0x05, composeBe(std::uint32_t{0x204})));
        h.transport.exchange(beef(0x06), beef_reply(0x06, composeBe(std::uint32_t{0x1000})));
        h.transport.exchange(beef(opcode), bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, opcode, 0x00});

        EXPECT_THAT(h.run(make_plan(operation, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed()); // no PROG_VOLT, no BLANK_PAGE
    }
}

TEST(SubaruDensoSh705xKlineExecutor, WriteThatStillDiffersAfterReflashFails)
{
    // Correction: legacy logged "ERROR IN FLASH PROCESS" and returned success.
    Harness h;
    const bytes::Bytes image = sh7055_image();
    const flashdev_t *device = find_flash_device("SH7055");
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {2});
    script_init(h.transport, 0x20);
    script_reflash(h.transport, image, device->fblocks[2], 0x24);
    script_compare(h.transport, "SH7055", image, {2});

    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.events.logs, ::testing::Contains(std::pair<LogLevel, std::string>{LogLevel::Error,
                                                                                    "*** ERROR IN FLASH PROCESS ***"}));
}

TEST(SubaruDensoSh705xKlineExecutor, ShortCrcReplyIsRejectedBeforeParsing)
{
    // Correction: legacy read at(5..8) after checking only `> 5`.
    Harness h;
    const bytes::Bytes image = sh7055_image();
    script_session(h.transport);
    h.transport.exchange(crc_request(find_flash_device("SH7055")->fblocks[0]),
                         bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x42, 0x12, 0x00});
    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed()); // no flush read, no second CRC request
}

TEST(SubaruDensoSh705xKlineExecutor, EachRejectedWriteStepStopsLaterCommands)
{
    // Reject PROG_VOLT, BLANK_PAGE, the first WRITE_FLASH_BUFFER, then COMMIT.
    const flashdev_t *device = find_flash_device("SH7055");
    const flashblock block = device->fblocks[0];
    const bytes::Bytes image = sh7055_image();
    const std::uint32_t crc0 = fastecu::checksum::crc32(bytes::ByteView(image).subspan(0, 0x1000));
    const std::vector<std::vector<std::pair<bytes::Bytes, bytes::Bytes>>> prefixes{
        {{beef(0x04), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00}}},
        {{beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0})},
         {beef(0x25, composeBe(block.start)), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00}}},
        {{beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0})},
         {beef(0x25, composeBe(block.start)), beef_reply(0x25)},
         {beef(0x22, composeBe(std::uint32_t{0}, bytes::ByteView(image).subspan(0, 0x200))),
          bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, 0x22, 0x00}}},
    };
    for (const auto& prefix : prefixes)
    {
        SCOPED_TRACE(prefix.size());
        Harness h;
        script_session(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        script_init(h.transport, 0x20);
        for (const auto& [request, reply] : prefix)
        {
            h.transport.exchange(request, reply);
        }
        EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed());
    }
    {
        Harness h;
        script_session(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        script_init(h.transport, 0x20);
        h.transport.exchange(beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0}));
        h.transport.exchange(beef(0x25, composeBe(block.start)), beef_reply(0x25));
        for (std::uint32_t address = 0; address < 0x1000; address += 0x200)
        {
            h.transport.exchange(beef(0x22, composeBe(address, bytes::ByteView(image).subspan(address, 0x200))),
                                 beef_reply(0x22, bytes::Bytes{0x00}));
        }
        h.transport.exchange(beef(0x24, composeBe(std::uint32_t{0}, std::uint16_t{0x1000}, crc0)),
                             bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, 0x24, 0x00});
        EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xKlineExecutor, CompareUsesLegacyTimeoutsAndPacing)
{
    struct RecordingClock final : FakeClock
    {
        Status sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
        {
            sleeps.push_back(duration);
            return FakeClock::sleep(duration, cancellation);
        }
        std::vector<std::chrono::milliseconds> sleeps;
    };
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    const bytes::Bytes image = sh7055_image();
    script_probe_alive(transport);
    script_compare(transport, "SH7055", image, {});
    RecordingClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    ASSERT_THAT(executor.execute(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image),
                                 transport, clock, cancellation, events),
                IsOk());
    // probe: 100 settle + 200 kernel-ID settle; then 16 x 5ms block pacing.
    std::vector<std::chrono::milliseconds> expected_sleeps{100ms, 200ms};
    expected_sleeps.insert(expected_sleeps.end(), 16, 5ms);
    EXPECT_EQ(clock.sleeps, expected_sleeps);
    std::vector<std::chrono::milliseconds> expected_reads{800ms};
    for (int i = 0; i < 16; ++i)
    {
        expected_reads.push_back(3000ms);
        expected_reads.push_back(200ms);
    }
    EXPECT_EQ(transport.read_timeouts_, expected_reads);
}

TEST(SubaruDensoSh705xKlineExecutor, WriteStepsUseLegacyTimeoutsWithoutSettleDelays)
{
    // init_flash_write():905/947/1000 500ms; reflash_block():1080 500ms;
    // flash_block():1171/1231/1335 3000ms; the delay(500)/(50)/(200) there are
    // commented out in legacy and stay omitted.
    struct RecordingClock final : FakeClock
    {
        Status sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
        {
            sleeps.push_back(duration);
            return FakeClock::sleep(duration, cancellation);
        }
        std::vector<std::chrono::milliseconds> sleeps;
    };
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    const bytes::Bytes image = sh7055_image();
    script_probe_alive(transport);
    script_compare(transport, "SH7055", image, {0});
    script_init(transport, 0x20);
    script_reflash(transport, image, find_flash_device("SH7055")->fblocks[0], 0x24);
    script_compare(transport, "SH7055", image, {});
    RecordingClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    ASSERT_THAT(executor.execute(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image),
                                 transport, clock, cancellation, events),
                IsOk());
    std::vector<std::chrono::milliseconds> expected_sleeps{100ms, 200ms};
    expected_sleeps.insert(expected_sleeps.end(), 32, 5ms); // two compares, no write-path delays
    EXPECT_EQ(clock.sleeps, expected_sleeps);
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
    EXPECT_EQ(transport.read_timeouts_, expected_reads);
}

TEST(SubaruDensoSh705xKlineExecutor, WriteLogsTheLegacyStrings)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    script_probe_alive(h.transport);
    script_compare(h.transport, "SH7055", image, {0});
    script_init(h.transport, 0x20);
    script_reflash(h.transport, image, find_flash_device("SH7055")->fblocks[0], 0x24);
    script_compare(h.transport, "SH7055", image, {});
    ASSERT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)), IsOk());

    using L = LogLevel;
    using Entry = std::pair<LogLevel, std::string>;
    const std::uint32_t crc0 = fastecu::checksum::crc32(bytes::ByteView(image).subspan(0, 0x1000));
    const std::uint32_t crc1 = fastecu::checksum::crc32(bytes::ByteView(image).subspan(0x1000, 0x1000));
    // write_mem():661-662, get_changed_blocks():782, check_romcrc():864-878
    // for the first two blocks (block 0 differs).
    const std::vector<Entry> compare_head{
        {L::Info, "Writing ROM to Subaru 04 32-bit using K-Line"},
        {L::Info, "--- Comparing ECU flash memory pages to image file ---"},
        {L::Info, "blk\tstart\tlen\tecu crc\timg crc\tsame?"},
        {L::Info, "FB00\t0x00000000\t0x00001000"},
        {L::Debug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", crc0 ^ 0xFFFFFFFFU, crc0)},
        {L::Info, std::format("\t{:08X}\t{:08X}", crc0 ^ 0xFFFFFFFFU, crc0)},
        {L::Info, "\tNO"},
        {L::Info, "FB01\t0x00001000\t0x00001000"},
        {L::Debug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", crc1, crc1)},
        {L::Info, std::format("\t{:08X}\t{:08X}", crc1, crc1)},
        {L::Info, "\tYES"},
    };
    const auto head = std::ranges::search(h.events.logs, compare_head);
    EXPECT_FALSE(head.empty());

    // write_mem():671-680, 695; init_flash_write():893-1008;
    // reflash_block():1065-1089, 1115; flash_block():1148-1310; write_mem():709.
    std::vector<Entry> write_body{
        {L::Info, "Different blocks : "},
        {L::Info, "0, "},
        {L::Info, " (total: 1)"},
        {L::Info, "--- Start writing ROM file to ECU flash memory ---"},
        {L::Info, "Check max message length"},
        {L::Info, ": 0x0204"},
        {L::Info, "Check flashblock size"},
        {L::Info, ": 0x1000"},
        {L::Info, "Test write mode off, perform actual flash write"},
        {L::Error, "Flash mode succesfully set"}, // legacy emits this through LOG_E
        {L::Info, "Flash block addr: 0x00000000 len: 0x00001000"},
        {L::Info, "Check flash voltage"},
        {L::Info, ": 18V"},
        {L::Info, "Flash page erase addr: 0x00000000 len: 0x00001000"},
        {L::Info, "Erasing flash page..."},
        {L::Info, " erased"},
        {L::Info, "Start flash write addr: 0x00000000 len: 0x00001000"},
    };
    for (std::uint32_t address = 0; address < 0x1000; address += 0x200)
    {
        write_body.emplace_back(L::Debug, "Data written to flash buffer");
        // FakeClock does not advance: 1ms per chunk, 0x200 * 1000 B/s, ~1 s.
        write_body.emplace_back(L::Info, std::format("Write flash buffer: 0x{:08X} ({}% - 512000 B/s, ~ 1 s remain)",
                                                     address, 100U * address / 0x1000U));
    }
    write_body.insert(write_body.end(),
                      {
                          {L::Info, "Flash buffer write complete... "},
                          {L::Debug, std::format("Image CRC32: 0x{:x}", crc0)},
                          {L::Info, "Committ flash addr: 0x0"},
                          {L::Info, " len: 0x1000"},
                          {L::Info, std::format(" crc32: 0x{:x}", crc0)},
                          {L::Info, "Flash block ok"},
                          {L::Info, "Block 0 reflash complete."},
                          {L::Info, "--- Comparing ECU flash memory pages to image file after reflash ---"},
                      });
    const auto body = std::ranges::search(h.events.logs, write_body);
    EXPECT_FALSE(body.empty());
    EXPECT_EQ(h.events.logs.back(), (Entry{L::Info, " (total: 0)"}));
    EXPECT_THAT(h.events.notices, ::testing::ElementsAre("Writing ROM, please wait..."));
}

TEST(SubaruDensoSh705xKlineExecutor, FailedFlashBlockLogsTheLegacyRecoveryText)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    script_probe_alive(h.transport);
    script_compare(h.transport, "SH7055", image, {0});
    script_init(h.transport, 0x20);
    h.transport.exchange(beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0}));
    h.transport.exchange(beef(0x25, composeBe(std::uint32_t{0})), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::BadResponse));
    ASSERT_GE(h.events.logs.size(), 3U);
    // flash_block():1180-1182, reflash_block():1109-1111, write_mem():703.
    EXPECT_EQ(h.events.logs[h.events.logs.size() - 3].first, LogLevel::Error);
    EXPECT_TRUE(h.events.logs[h.events.logs.size() - 3].second.starts_with("Wrong response from ECU: "));
    EXPECT_EQ(h.events.logs[h.events.logs.size() - 2],
              (std::pair<LogLevel, std::string>{LogLevel::Error,
                                                "Reflash error! Do not panic, do not reset the ECU immediately. The "
                                                "kernel is most likely still running and receiving commands!"}));
    EXPECT_EQ(h.events.logs.back(), (std::pair<LogLevel, std::string>{LogLevel::Info, "Block 0 reflash failed."}));
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
        const bytes::Bytes image = sh7055_image();
        script_probe_alive(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        script_init(h.transport, 0x20);
        script_reflash(h.transport, image, find_flash_device("SH7055")->fblocks[0], 0x24);
        script_compare(h.transport, "SH7055", image, {});
        h.cancellation.set_predicate([&h, k] { return h.transport.writesConsumed() >= k; });
        EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(h.transport.writesConsumed(), k);
    }
}

} // namespace
} // namespace fastecu::flash
