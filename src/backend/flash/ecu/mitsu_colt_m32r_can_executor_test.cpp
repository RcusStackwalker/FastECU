// Equivalence tests for MitsuColtM32rCanExecutor, the portable replacement for
// FlashEcuMitsuM32rCanOperation's connect_bootloader(), readFlashRange(),
// read_mem(), upload_and_commit(), ensureTopRegionWritten() and write_mem().
// Every expected request below is built with the same
// MitsuColtCan/MitsuColtCanVendorExt builders the legacy class calls (through
// its qt_colt.h *Frame shims), and every expected log string is copied
// character-for-character from
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.cpp.
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/flash_cancellation.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::flash::build_mitsu_colt_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanExecutor;
using fastecu::flash::ScriptedCanFlashTransport;
using testing::Contains;
using testing::Each;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using testing::Pair;

constexpr std::string_view kProtocol = "mitsu_ecu_m32r_can";
// flashdevices["M32R_128KB"].fblocks[0] is {0x00000000, 0x00010000}
// (kernelmemorymodels.h:362-365) -- a 64KB first block, which is what the
// read plan transfers and what keeps the scripted read to 342 chunks.
constexpr std::string_view kMcu = "M32R_128KB";

// Every request on this bus carries a 4-byte big-endian 0x7E0 prefix
// (legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64).
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Responses carry the 4-byte 0x7E8 reply id; the legacy code indexes
// received.at(4) for the service byte throughout.
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

fastecu::flash::FlashPlan readPlan(bool vendor = false)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, vendor,
                                               std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The ROM image every write test writes: kTopRegionEnd bytes, 0x00 below
// kTopRegionStart and 0xEE above it. The distinctive top-region fill is what
// makes the bootstrap comparison observable -- an ECU that reports 0xEE
// matches, one that reports 0xFF (erased flash) does not.
bytes::Bytes writeRom()
{
    bytes::Bytes rom(MitsuColtCan::kTopRegionEnd, 0x00);
    std::fill(rom.begin() + MitsuColtCan::kTopRegionStart, rom.end(), 0xEE);
    return rom;
}

// A Write plan is what selects kSessionBootload, and kSessionBootload is the
// only thing that reaches connect_bootloader()'s factory SecurityAccess arm
// (legacy lines 131-165). build_mitsu_colt_m32r_can_plan always declares both
// ConfirmationSpecs for a Write, so a plan from here is fully gated.
fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                               /*use_vendor_challenge=*/false, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_mitsu_colt_m32r_can_plan: the
// builder always declares both confirmations, and validate_and_build does not
// require them, so this is the only way to reach the executor with a Write
// plan whose high-risk step was never granted.
fastecu::flash::FlashPlan writePlanGranting(
    std::initializer_list<fastecu::flash::ConfirmationSpec::Id> granted,
    bytes::Bytes rom = writeRom(), FlashOperation operation = FlashOperation::Write)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::MitsuColtM32rCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region =
        fastecu::flash::MemoryRegion{MitsuColtCan::kUserspaceStart,
                                     MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart};
    fields.image = std::move(rom);
    fields.family_plan = fastecu::flash::MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = false,
        .session_id = MitsuColtCan::kSessionBootload,
    };
    for (const fastecu::flash::ConfirmationSpec::Id id : granted)
    {
        fields.confirmations.push_back(fastecu::flash::ConfirmationSpec{id, {}});
    }
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The 4 seed bytes the ECU returns in the tests below. Deliberately all
// distinct and distinct from the surrounding framing bytes, so that reading
// the seed from any offset other than the legacy `received.mid(6, 4)` yields
// a different seed -- and therefore a different key on the wire, which the
// scripted transport rejects.
constexpr bytes::Byte kSeed[] = {0x11, 0x22, 0x33, 0x44};

// Scripts the chunked ReadMemoryByAddress sweep over [start, start+length),
// filling every payload with `fill`.
void scriptFlashRead(ScriptedCanFlashTransport& transport, std::uint32_t start,
                     std::uint32_t length, bytes::Byte fill)
{
    for (std::uint32_t addr = start; addr < start + length;
         addr += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = response({0x63});
        reply.insert(reply.end(), chunk, fill);
        transport.queueRead(reply);
    }
}

// Scripts the full sweep the executor must perform over the plan's transfer
// region.
void scriptFullRead(ScriptedCanFlashTransport& transport, const fastecu::flash::FlashPlan& plan,
                    bytes::Byte fill)
{
    scriptFlashRead(transport, plan.transfer_region().start, plan.transfer_region().length, fill);
}

// Scripts the bootload handshake a Write plan drives: session 0x85 then the
// factory SecurityAccess seed/key pair (legacy lines 119-165).
void scriptBootloadHandshake(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    transport.expectWrite(
        request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed))));
    transport.queueRead(response({0x67, 0x06}));
}

// Scripts one upload_and_commit(start, data): RequestDownload, the
// TransferData chunks, the CRC RequestDownload + TransferData, and the
// RoutineControl CRC check (legacy lines 231-297).
void scriptUploadAndCommit(ScriptedCanFlashTransport& transport, std::uint32_t start,
                           bytes::ByteView data)
{
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        start, static_cast<std::uint32_t>(data.size()))));
    transport.queueRead(response({0x74}));

    for (const bytes::Bytes& chunk : MitsuColtCan::buildTransferDataFrames(data))
    {
        transport.expectWrite(request(chunk));
        transport.queueRead(response({0x76}));
    }

    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize)));
    transport.queueRead(response({0x74}));

    const std::uint16_t crc = MitsuColtCan::checksum(data);
    const bytes::Bytes crcData{static_cast<bytes::Byte>((crc >> 8) & 0xff),
                               static_cast<bytes::Byte>(crc & 0xff)};
    transport.expectWrite(request(MitsuColtCan::buildTransferDataFrames(crcData).front()));
    transport.queueRead(response({0x76}));

    transport.expectWrite(request(MitsuColtCan::buildRoutineCheckCrc(start)));
    transport.queueRead(response({0x71}));
}

// Scripts the unlock + erase-trigger pair (legacy lines 349-368 / 445-464).
void scriptUnlockAndErase(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x71}));
}

// The userspace slice of `rom` the write path must transfer.
bytes::ByteView userspaceOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + MitsuColtCan::kUserspaceStart,
                           MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart);
}

// The top-region slice of `rom` the bootstrap must write and verify.
bytes::ByteView topRegionOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + MitsuColtCan::kTopRegionStart,
                           MitsuColtCan::kTopRegionLength);
}

TEST(MitsuColtM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    // A plan built for another family must be rejected by
    // check_family_transport_match before configure()/open() or any write --
    // the scripted transport is left completely untouched, which is the
    // assertion that matters here.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Hand-built rather than produced by a builder: the point is a plan this
    // executor must refuse, and only validate_and_build can make a FlashPlan.
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

TEST(MitsuColtM32rCanExecutor, ReadPerformsTheBasicHandshakeThenChunkedReads)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    // Legacy: connect_bootloader() sends SID 0x10 with kSessionBasic and
    // requires (0x10+0x40, 0x81) back (lines 119-129).
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    // 64KB at kFlashReadBlockSize (192) per chunk.
    scriptFullRead(transport, plan, 0xAB);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), plan.transfer_region().length);
    EXPECT_THAT(*result->read_bytes, Each(0xAB));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Diagnostic session ok")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "ROM read complete")));
    EXPECT_THAT(events.notices, Contains("Reading ROM, please wait..."));
    // The bus configuration comes from the plan, not from a hardcoded literal.
    ASSERT_TRUE(transport.last_config_.has_value());
    EXPECT_EQ(transport.last_config_->request_id, 0x7e0u);
    EXPECT_EQ(transport.last_config_->response_id, 0x7e8u);
    EXPECT_EQ(transport.last_config_->bitrate, 500000);
    EXPECT_FALSE(transport.last_config_->extended_id);
}

TEST(MitsuColtM32rCanExecutor, ReadRejectsAWrongDiagnosticSessionResponse)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x7f, 0x10, 0x12})); // negative response

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:126. The NRC context
    // is the whole tail from the service byte on (QByteArray::mid clamps its
    // length argument), so the 0x12 NRC still decodes -- asserted exactly,
    // because a one-byte-short context would silently degrade this to
    // "Not a valid answer".
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Wrong response from ECU: Subfunction not supported")));
}

TEST(MitsuColtM32rCanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queue_no_frame();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
}

TEST(MitsuColtM32rCanExecutor, ReadPropagatesADisconnectedTransport)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(MitsuColtM32rCanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));
    cancellation.trip();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

// Trips a cancellation source as soon as the first chunk's progress is
// reported, so what stops the read is the loop's own top-of-chunk
// cancellation check (legacy readFlashRange line 183) rather than a token
// that was already cancelled before execute() was called.
class CancelAfterFirstChunkSink final : public RecordingEventSink
{
  public:
    explicit CancelAfterFirstChunkSink(fastecu::flash::CancellationSource& source)
        : source_(source)
    {
    }
    void progress(int done, int total) override
    {
        RecordingEventSink::progress(done, total);
        if (done > 0)
        {
            source_.trip();
        }
    }

  private:
    fastecu::flash::CancellationSource& source_;
};

TEST(MitsuColtM32rCanExecutor, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    fastecu::flash::CancellationSource cancellation;
    CancelAfterFirstChunkSink events{cancellation};
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    // Exactly one read chunk is scripted; the executor is cancelled while it
    // is being served, so the loop must stop at the top of the next chunk.
    const std::uint32_t start = plan.transfer_region().start;
    transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(
        start, static_cast<bytes::Byte>(MitsuColtCan::kFlashReadBlockSize))));
    bytes::Bytes reply = response({0x63});
    reply.insert(reply.end(), MitsuColtCan::kFlashReadBlockSize, 0x5A);
    transport.queueRead(reply);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, VendorChallengePrecedesTheDiagnosticSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(/*vendor=*/true);

    // Legacy ordering, flash_ecu_mitsu_m32r_can_operation.cpp:84-129:
    // seed request, key answer, then the diagnostic session.
    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x63, 0x27, 0x41, 0x12, 0x34, 0x56, 0x78}));

    const std::uint32_t key = MitsuColtCanVendorExt::challengeInverseTransform(0x12345678);
    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
    transport.queueRead(response({0x63, 0x27, 0x42}));

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queue_no_frame(); // stop here: ordering is what this pins

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Vendor challenge accepted")));
    // SsmProtocol::toHex is lowercase "%02x " per byte, trailing space included.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Received vendor seed: 12 34 56 78 ")));
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeRejectionStopsBeforeTheSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(/*vendor=*/true);

    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x7f, 0x23, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:93, with the NRC
    // 0x33 decoded from the untruncated context.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Wrong vendor challenge response from ECU: "
                                               "Security access denied")));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, ReadEmitsMonotonicProgress)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    scriptFullRead(transport, plan, 0x00);

    ASSERT_TRUE(executor.execute(plan, transport, clock, cancellation.token(), events)
                    .has_value());

    const auto length = static_cast<int>(plan.transfer_region().length);
    ASSERT_FALSE(events.progress_calls.empty());
    EXPECT_EQ(events.progress_calls.front(), std::make_pair(0, length));
    EXPECT_EQ(events.progress_calls.back().first, length);
    for (std::size_t i = 1; i < events.progress_calls.size(); ++i)
    {
        EXPECT_GE(events.progress_calls[i].first, events.progress_calls[i - 1].first);
        EXPECT_EQ(events.progress_calls[i].second, length);
    }
}

TEST(MitsuColtM32rCanExecutor, WriteDrivesTheBootloadSessionThenFactorySecurityAccess)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = writePlan(writeRom());

    // Legacy line 82: a write selects kSessionBootload (0x85), not kSessionBasic.
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    // Legacy lines 136-145: SID 0x27/5, answered with (0x27+0x40, 0x05) and a
    // 4-byte seed at received.mid(6, 4).
    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    // Legacy lines 151-156: the key is seedKey(seed), and the request carries
    // it verbatim. Scripting the exact bytes means a wrong seed offset or a
    // wrong key derivation is rejected by the transport, not silently accepted.
    transport.expectWrite(
        request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed))));
    transport.queueRead(response({0x67, 0x06}));

    // The handshake is what this test pins, so the run is stopped one request
    // into the write path: the first chunk of the top-region check goes
    // unanswered. That the request is a top-region read at all is the proof
    // the write branch now proceeds instead of refusing.
    transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(
        MitsuColtCan::kTopRegionStart,
        static_cast<bytes::Byte>(MitsuColtCan::kFlashReadBlockSize))));
    transport.queue_no_frame();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Checking top 128KB (0x60000-0x80000)...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Diagnostic session ok")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Requesting security seed...")));
    // Pins both the mid(6, 4) offset and SsmProtocol::toHex's exact format.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Received seed: 11 22 33 44 ")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, HasSubstr("Calculated seed key: "))));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Sending seed key to ECU...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Security access ok")));
}

TEST(MitsuColtM32rCanExecutor, WriteRejectsASecuritySeedWithTheWrongSubfunction)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = writePlan(writeRom());

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    // Long enough and the right service, but subfunction 0x04 instead of the
    // 0x05 legacy line 141 demands -- so the seed must be refused rather than
    // read out of a frame that never carried one.
    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x04, 0x11, 0x22, 0x33, 0x44}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed()); // the key exchange never happens
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:143.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Wrong response from ECU: Not a valid answer")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Security access ok"))));
}

TEST(MitsuColtM32rCanExecutor, WriteRejectsASecurityKeyAnswerWithTheWrongSubfunction)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = writePlan(writeRom());

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    // Right service, but the seed-request subfunction echoed back instead of
    // the 0x06 legacy line 160 demands.
    transport.expectWrite(
        request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed))));
    transport.queueRead(response({0x67, 0x05}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:162.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Wrong response from ECU: Not a valid answer")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Security access ok"))));
}

TEST(MitsuColtM32rCanExecutor, WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Top region in the ROM image is all 0xEE, and the ECU reports 0xEE too.
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Top 128KB already matches, no bootstrap needed")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Erase page uploaded")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Write page uploaded")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash erased")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Writing ROM userspace 0x8000-0x60000...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash written")));
    // Nothing from the bootstrap arm ran.
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info, "Top 128KB written via redirect"))));
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
}

TEST(MitsuColtM32rCanExecutor, WriteRunsTheBootstrapWhenTheTopRegionDiffers)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    // ECU reports 0xFF: mismatch, so the bootstrap runs.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    scriptUnlockAndErase(transport);
    // The carrier address is kUserspaceStart, not kTopRegionStart: the redirect
    // routines add the +0x058000 offset themselves.
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, topRegionOf(rom));
    // Verify read-back returns what was written.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    // Then the ordinary write proceeds.
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info,
                              "Top 128KB mismatch, bootstrapping via redirect routines...")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info,
                              "Uploading erase redirect routine to RAM 0x805568...")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info,
                              "Uploading write redirect routine to RAM 0x8054ac...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Carrier window erased")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Top 128KB written via redirect")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Top 128KB verified")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash written")));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheTopRegionVerifyMismatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, topRegionOf(rom));
    // Verify read-back still reports 0xFF: the write did not take.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Top 128KB verify failed after redirect write")));
    // The main write never starts.
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Erase page uploaded"))));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheReflashUnlockIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7f, 0x3b, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // The erase trigger never goes out.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:451.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Reflash unlock rejected: Security access denied")));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheEraseTriggerIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x7f, 0x31, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // No RequestDownload for the ROM userspace follows a refused erase.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:461.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Erase trigger rejected: Conditions not correct")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash erased"))));
}

TEST(MitsuColtM32rCanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // build_mitsu_colt_m32r_can_plan refuses TestWrite, but validate_and_build
    // accepts it -- and a dry run must never reach the erase trigger.
    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::EraseTrigger,
                                   fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap},
                                  writeRom(), FlashOperation::TestWrite);

    scriptBootloadHandshake(transport);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Not(Contains("Writing ROM, please wait...")));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesAnImageShorterThanTheTopRegion)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // build_mitsu_colt_m32r_can_plan rejects this image, but validate_and_build
    // does not -- and every slice the write path takes would be out of bounds.
    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::EraseTrigger,
                                   fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap},
                                  bytes::Bytes(MitsuColtCan::kTopRegionEnd - 1, 0x00));

    scriptBootloadHandshake(transport);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:400.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "ROM file too small: need at least 0x80000 bytes")));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesTheBootstrapWhenItsConfirmationIsAbsent)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Everything but the bootstrap gate is granted, and the top region does
    // not match -- so the run must stop at the gate rather than firing the
    // redirect routines.
    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::EraseTrigger});

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:331.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Top 128KB bootstrap canceled by user")));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info,
                                  "Uploading erase redirect routine to RAM 0x805568..."))));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesTheEraseTriggerWhenItsConfirmationIsAbsent)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap});

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    // The reflash-unlock payload is never scripted, so scriptConsumed() here is
    // the assertion that it never reached the bus.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:441.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Erase trigger canceled by user")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash erased"))));
}

} // namespace
