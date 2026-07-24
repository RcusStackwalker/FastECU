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

#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/ports/clock_test_helpers.h"
#include "tests/scripted_kline_flash_transport.h"

namespace fastecu::flash
{
namespace
{

class NeverCancelled : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

// Mirrors tests/test_ssm_logging_protocol.cpp's CancelsDuringSecondRead:
// returns false for the first `remaining` checks, then true forever.
class CancelsAfterNChecks final : public ICancellationToken
{
  public:
    explicit CancelsAfterNChecks(int remaining) : remaining_(remaining)
    {
    }
    bool cancelled() const override
    {
        if (remaining_ <= 0)
        {
            return true;
        }
        --remaining_;
        return false;
    }

  private:
    mutable int remaining_;
};

class RecordingEventSink : public IEventSink
{
  public:
    void log(LogLevel, std::string_view) override
    {
    }
    void progress(int done, int total) override
    {
        progress_calls.emplace_back(done, total);
    }
    void notice(std::string_view) override
    {
    }
    std::vector<std::pair<int, int>> progress_calls;
};

// Satisfies IFlashTransport (the lifetime/unblock-only base) but NOT
// IKlineFlashTransport -- used to prove execute()'s dynamic_cast guard
// rejects a wrong concrete transport type without doing any I/O. Task 9's
// CAN executor is expected to introduce a proper ScriptedCanFlashTransport;
// this bare stand-in exists only because this task must not depend on work
// Task 9 hasn't done yet.
class BareFlashTransport final : public IFlashTransport
{
  public:
    void request_unblock() noexcept override
    {
    }
};

// eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp:62-63 -- tester_id =
// 0xF0, target_id = 0x10, hardcoded in execute() and mirrored by the
// builder's DensoSh705xEepromKlinePlan.
constexpr std::uint8_t kTesterId = 0xf0;
constexpr std::uint8_t kTargetId = 0x10;

// Matches resources/shared/config/protocols.cfg's
// sub_ecu_eeprom_denso_sh7055_kline entry (kernel_addr = 0xFFFF6004).
constexpr std::uint32_t kKernelStartAddr = 0xFFFF6004;

// ---- Request builders: TRANSCRIBE of each send_sid_* body (see
// task-6-report.md's table for exact legacy line ranges). ----

bytes::Bytes sidBfSsmInitRequest()
{
    return SsmProtocol::addHeader(bytes::Bytes{0xbf}, kTesterId, kTargetId, false);
}
bytes::Bytes sid81StartCommRequest()
{
    return SsmProtocol::addHeader(bytes::Bytes{0x81}, kTesterId, kTargetId, false);
}
bytes::Bytes sid83TimingsRequest()
{
    return SsmProtocol::addHeader(bytes::Bytes{0x83, 0x00}, kTesterId, kTargetId, false);
}
bytes::Bytes sid27RequestSeedRequest()
{
    return SsmProtocol::addHeader(bytes::Bytes{0x27, 0x01}, kTesterId, kTargetId, false);
}
bytes::Bytes sid27SendKeyRequest(bytes::ByteView key)
{
    bytes::Bytes out{0x27, 0x02};
    out.insert(out.end(), key.begin(), key.end());
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}
bytes::Bytes sid10StartDiagRequest()
{
    return SsmProtocol::addHeader(bytes::Bytes{0x10, 0x85, 0x02}, kTesterId, kTargetId, false);
}
bytes::Bytes sid34RequestUploadRequest(std::uint32_t dataaddr, std::uint32_t datalen)
{
    bytes::Bytes out{
        0x34,
        static_cast<bytes::Byte>((dataaddr >> 16) & 0xFF),
        static_cast<bytes::Byte>((dataaddr >> 8) & 0xFF),
        static_cast<bytes::Byte>(dataaddr & 0xFF),
        0x04,
        static_cast<bytes::Byte>((datalen >> 16) & 0xFF),
        static_cast<bytes::Byte>((datalen >> 8) & 0xFF),
        static_cast<bytes::Byte>(datalen & 0xFF),
    };
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}
bytes::Bytes sid36TransferDataRequest(std::uint32_t blockaddr, bytes::ByteView blockBytes)
{
    bytes::Bytes out{
        0x36,
        static_cast<bytes::Byte>((blockaddr >> 16) & 0xFF),
        static_cast<bytes::Byte>((blockaddr >> 8) & 0xFF),
        static_cast<bytes::Byte>(blockaddr & 0xFF),
    };
    out.insert(out.end(), blockBytes.begin(), blockBytes.end());
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}
bytes::Bytes sid31StartRoutineRequest()
{
    return SsmProtocol::addHeader(bytes::Bytes{0x31, 0x01, 0x01}, kTesterId, kTargetId, false);
}

// request_kernel_id(), lines 964-994: NOT addHeader-framed.
bytes::Bytes requestKernelIdRequest()
{
    bytes::Bytes out{
        static_cast<bytes::Byte>((0xBEEF >> 8) & 0xFF),
        static_cast<bytes::Byte>(0xBEEF & 0xFF),
        0x00,
        0x01,
        0x01,
    };
    out.push_back(SsmProtocol::checksum(out, false));
    return out;
}

// generate_seed_key(), lines 854-879 (stock / non-"_ecutek" table pair).
bytes::Bytes generateSeedKey(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {
        0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608,
        0xAFB8, 0x503D, 0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0};
    static constexpr std::uint8_t kTransform[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

// encrypt_payload(), lines 923-939.
bytes::Bytes encryptPayload(bytes::ByteView buf, std::uint32_t len)
{
    static constexpr std::uint16_t kIndex[] = {0x7856, 0xCE22, 0xF513, 0x6E86};
    static constexpr std::uint8_t kTransform[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculatePayload(buf, len, kIndex, kTransform);
}

// For McuType "SH7055", eblocks_SH7055[0] == {start=0, len=0x100}
// (src/backend/definitions/kernelmemorymodels.h:279-281). read_mem()'s
// skip_start/willget/numblocks/curblock arithmetic (lines 469-510) reduces,
// for this start/length, to a single request with numblocks=8, curblock=0,
// for every EEPROM_MODE value.
bytes::Bytes sidDumpRequestForSh7055(std::uint8_t eepromMode)
{
    return {0xbd, eepromMode, 0x00, 0x08, 0x00, 0x00};
}

// read_mem()'s per-block unframing (lines 545-550): each 35-byte wire block
// is [2 prefix][32 data][1 trailing]; prefix/trailing are stripped and never
// inspected. 8 blocks * 35 bytes = 280 bytes.
bytes::Bytes eepromPayload280Bytes()
{
    bytes::Bytes out;
    for (int block = 0; block < 8; ++block)
    {
        out.push_back(0xEE);
        out.push_back(0xEE);
        for (int j = 0; j < 32; ++j)
        {
            out.push_back(static_cast<bytes::Byte>((block * 32 + j) & 0xFF));
        }
        out.push_back(0xFF);
    }
    return out; // 280 bytes
}

bytes::Bytes expectedDecodedEeprom256Bytes()
{
    bytes::Bytes out;
    for (int i = 0; i < 256; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i & 0xFF));
    }
    return out;
}

// Minimal positive-response fixture: only byte index 4 (the service/session
// code) is inspected for these SIDs, so a 5-byte frame with that one byte
// set is sufficient and unambiguous with the "no frame"/too-short checks.
bytes::Bytes positiveResponse(std::uint8_t serviceCode)
{
    bytes::Bytes out(5, 0);
    out[4] = serviceCode;
    return out;
}

// send_sid_bf_ssm_init()'s response: connect_bootloader() requires byte[4]
// == 0xFF; content beyond that is logged, never asserted, so 13 bytes of
// arbitrary filler (>= the 13 needed to avoid a negative-length strip in the
// legacy Qt code) is sufficient here too.
bytes::Bytes sidBfSsmInitResponse()
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
bytes::Bytes sid27SeedResponse(bytes::ByteView seed)
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
bytes::Bytes kernelAliveResponse()
{
    bytes::Bytes out{
        static_cast<bytes::Byte>((0xBEEF >> 8) & 0xFF),
        static_cast<bytes::Byte>(0xBEEF & 0xFF),
        0x00,
        0x06,
        0x41,
        'K',
        'E',
        'R',
        'N',
        '2',
    };
    return out; // 10 bytes
}

// A 16-byte (already 4-byte-aligned) kernel fixture -- matches
// send_sid_36_transferdata()'s single-block path exactly, and (per
// task-6-report.md's "Legacy behavior surprises" #1) is already aligned so
// this executor's OOB-read-avoidance padding is a documented no-op here.
bytes::Bytes kernelFixtureBytes()
{
    bytes::Bytes out;
    for (int i = 0; i < 16; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i));
    }
    return out;
}

// Enqueues the exact write/read sequence for one full "kernel not yet
// running" round: connect_bootloader()'s non-alive probe + full bf/81/83/
// 27/27/10 init, then upload_kernel()'s 34/36/34/36/31 sequence and its
// kernel-alive re-poll. 13 writes total.
void enqueueFullBootloaderAndKernelUpload(ScriptedKlineFlashTransport& transport, bytes::ByteView seed,
                                          bytes::ByteView kernelBytes, std::uint32_t kernelStartAddr)
{
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame(); // kernel not (yet) alive

    transport.expectWrite(sidBfSsmInitRequest());
    transport.queueRead(sidBfSsmInitResponse());

    transport.expectWrite(sid81StartCommRequest());
    transport.queueRead(positiveResponse(0xC1));

    transport.expectWrite(sid83TimingsRequest());
    transport.queueRead(positiveResponse(0xC3));

    transport.expectWrite(sid27RequestSeedRequest());
    transport.queueRead(sid27SeedResponse(seed));

    const bytes::Bytes seedKey = generateSeedKey(seed);
    transport.expectWrite(sid27SendKeyRequest(seedKey));
    transport.queueRead(positiveResponse(0x67));

    transport.expectWrite(sid10StartDiagRequest());
    transport.queueRead(positiveResponse(0x50));

    const std::uint32_t plLen = (static_cast<std::uint32_t>(kernelBytes.size()) + 3) & ~std::uint32_t(3);
    transport.expectWrite(sid34RequestUploadRequest(kernelStartAddr, plLen));
    transport.queueRead(positiveResponse(0x74));

    // Pad to plLen with zero bytes before encrypting -- this is the
    // executor's OOB-read fix (denso_sh705x_eeprom_kline_executor.cpp,
    // upload_kernel()), not the legacy behavior. For an already-aligned
    // kernelBytes (kernelFixtureBytes(), used by most tests here) this is a
    // no-op; NonAlignedKernelIsPaddedBeforeEncryptionAvoidsOobRead below
    // exercises the case where padding actually changes the wire bytes.
    bytes::Bytes paddedKernel(kernelBytes.begin(), kernelBytes.end());
    paddedKernel.resize(plLen, 0);
    transport.expectWrite(sid36TransferDataRequest(kernelStartAddr, encryptPayload(paddedKernel, plLen)));
    transport.queueRead(positiveResponse(0x76));

    const bytes::Bytes cksBypass{0x00, 0x00, 0x5A, 0xA5};
    transport.expectWrite(sid34RequestUploadRequest(kernelStartAddr + plLen, 4));
    transport.queueRead(positiveResponse(0x74));

    transport.expectWrite(sid36TransferDataRequest(kernelStartAddr + plLen, encryptPayload(cksBypass, 4)));
    transport.queueRead(positiveResponse(0x76));

    transport.expectWrite(sid31StartRoutineRequest());
    transport.queueRead(positiveResponse(0x71));

    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
}

Result<FlashPlan> makeKlinePlan(EepromReadMode mode, bytes::Bytes kernelBytes, std::uint32_t kernelAddr)
{
    return build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = kernelAddr, .bytes = std::move(kernelBytes)},
        .mode = mode,
        .security = DensoSecurityVariant::Stock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
}

Result<FlashPlan> valid_kline_plan(EepromReadMode mode = EepromReadMode::Mode2)
{
    return makeKlinePlan(mode, kernelFixtureBytes(), kKernelStartAddr);
}

} // namespace

TEST(DensoSh705xEepromKlineExecutorTest, WrongFamilyPlanIsRejectedWithNoTransportCalls)
{
    auto plan = build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromCan,
        .target_id = "sub_ecu_eeprom_denso_sh7058_can",
        .mcu_name = "SH7058",
        .flash_method = "sub_ecu_eeprom_denso_sh7058_can",
        .kernel = KernelImage{.id = "k", .load_address = 0xFFFF3000, .bytes = {0x01, 0x02, 0x03, 0x04}},
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
    ASSERT_TRUE(plan.has_value());

    DensoSh705xEepromKlineExecutor executor;
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(transport.scriptConsumed()); // nothing was ever queued or consumed
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(DensoSh705xEepromKlineExecutorTest, FullBootloaderStockSecurityMode2MatchesLegacyTrace)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    enqueueFullBootloaderAndKernelUpload(transport, seed, kernelFixtureBytes(), kKernelStartAddr);
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(DensoSh705xEepromKlineExecutorTest, KernelAlreadyRunningSkipsBootloaderMatchesLegacyTrace)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
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
    bytes::Bytes kernel15;
    for (int i = 0; i < 15; ++i)
    {
        kernel15.push_back(static_cast<bytes::Byte>(i));
    }
    auto plan = makeKlinePlan(EepromReadMode::Mode2, kernel15, kKernelStartAddr);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    enqueueFullBootloaderAndKernelUpload(transport, seed, kernel15, kKernelStartAddr);
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(DensoSh705xEepromKlineExecutorTest, NoResponseAtHandshakeReturnsTimeout)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame(); // kernel not (yet) alive
    transport.expectWrite(sidBfSsmInitRequest());
    transport.queue_no_frame(); // no response at all -> Timeout

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(DensoSh705xEepromKlineExecutorTest, TransportReportsClosedReturnsDisconnected)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.open_result_ = fail(ErrorKind::Disconnected, "port not open");

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());   // zero writes attempted
    EXPECT_EQ(transport.close_call_count_, 0); // open() itself failed -- ScopedClose never engages
}

TEST(DensoSh705xEepromKlineExecutorTest, MalformedSid81ResponseReturnsBadResponse)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame();
    transport.expectWrite(sidBfSsmInitRequest());
    transport.queueRead(sidBfSsmInitResponse());
    transport.expectWrite(sid81StartCommRequest());
    transport.queueRead(positiveResponse(0x00)); // byte[4] != 0xC1

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(DensoSh705xEepromKlineExecutorTest, CancellationDuringKernelUploadReturnsCancelled)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    enqueueFullBootloaderAndKernelUpload(transport, seed, kernelFixtureBytes(), kKernelStartAddr);
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());

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
    CancelsAfterNChecks cancellation(37);
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.close_call_count_, 1);
    // Concretely proves "between chunks": connect_bootloader's 7 writes
    // (probe + bf/81/83/27req/27key/10) plus upload_kernel's kernel-upload
    // request (sid_34) happened -- 8 total -- but the kernel-data chunk
    // (sid_36) was never written.
    EXPECT_EQ(transport.writesConsumed(), 8u);
}

TEST(DensoSh705xEepromKlineExecutorTest, CloseFailureAfterSuccessfulReadIsReturned)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());
    transport.close_result_ = fail(ErrorKind::Internal, "close failed");

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(DensoSh705xEepromKlineExecutorTest, OriginalErrorWinsOverCloseFailure)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame();
    transport.expectWrite(sidBfSsmInitRequest());
    transport.queue_no_frame(); // Timeout at handshake
    transport.close_result_ = fail(ErrorKind::Internal, "close also failed");

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout); // original error wins
    EXPECT_EQ(transport.close_call_count_, 1);
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
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    enqueueFullBootloaderAndKernelUpload(transport, seed, kernelFixtureBytes(), kKernelStartAddr);
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expectedDecodedEeprom256Bytes());
    // false before connect_bootloader()/upload_kernel() (self-framed via
    // SsmProtocol::addHeader(), must not double-frame), true right before
    // read_mem()'s raw SID_DUMP requests, false again afterward so a shared,
    // session-lifetime serial instance isn't left dirtied for the next
    // unrelated operation.
    EXPECT_EQ(transport.header_mode_calls_, (std::vector<bool>{false, true, false}));
}

// Same assertion on the "kernel already running" path (connect_bootloader()
// short-circuits, upload_kernel() is skipped entirely) -- proves the OFF ->
// ON -> OFF sequence doesn't depend on the upload phase actually running.
TEST(DensoSh705xEepromKlineExecutorTest, HeaderModeSequenceHoldsWhenKernelAlreadyRunning)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queueRead(eepromPayload280Bytes());

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    EXPECT_EQ(transport.header_mode_calls_, (std::vector<bool>{false, true, false}));
}

// Proves the header is still forced back OFF even when read_mem() itself
// fails (e.g. a hard transport error mid-read) -- the reset is unconditional
// on the result, not just on the happy path, since a stuck `true` on a
// shared, session-lifetime serial instance would corrupt every subsequent
// unrelated K-Line operation the user runs.
TEST(DensoSh705xEepromKlineExecutorTest, HeaderModeResetToOffEvenWhenReadMemFails)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedKlineFlashTransport transport;
    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
    transport.expectWrite(sidDumpRequestForSh7055(2));
    transport.queue_error(ErrorKind::Disconnected, "port dropped mid-read");

    DensoSh705xEepromKlineExecutor executor;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(transport.header_mode_calls_, (std::vector<bool>{false, true, false}));
}

TEST(DensoSh705xEepromKlineExecutorTest, WrongConcreteTransportTypeReturnsInvalidConfigWithNoIo)
{
    auto plan = valid_kline_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    DensoSh705xEepromKlineExecutor executor;
    BareFlashTransport transport;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

} // namespace fastecu::flash
