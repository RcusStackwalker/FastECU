// Connect + read equivalence tests for MitsuColtM32rCanExecutor, the portable
// replacement for FlashEcuMitsuM32rCanOperation's connect_bootloader(),
// readFlashRange() and read_mem(). Every expected request below is built with
// the same MitsuColtCan/MitsuColtCanVendorExt builders the legacy class calls
// (through its qt_colt.h *Frame shims), and every expected log string is
// copied character-for-character from
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.cpp.
//
// The write path (write_mem/upload_and_commit/ensureTopRegionWritten) is
// deliberately not exercised here -- it lands in the next task.
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

// A Write plan is what selects kSessionBootload, and kSessionBootload is the
// only thing that reaches connect_bootloader()'s factory SecurityAccess arm
// (legacy lines 131-165). The write path itself is not implemented yet, so
// every test below drives the arm and then asserts the executor's final
// Unsupported -- the handshake is fully exercised before that point.
fastecu::flash::FlashPlan writePlan()
{
    auto plan = build_mitsu_colt_m32r_can_plan(
        FlashOperation::Write, kProtocol, kMcu, /*use_vendor_challenge=*/false,
        bytes::Bytes(MitsuColtCan::kTopRegionEnd, 0x00));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The 4 seed bytes the ECU returns in the tests below. Deliberately all
// distinct and distinct from the surrounding framing bytes, so that reading
// the seed from any offset other than the legacy `received.mid(6, 4)` yields
// a different seed -- and therefore a different key on the wire, which the
// scripted transport rejects.
constexpr bytes::Byte kSeed[] = {0x11, 0x22, 0x33, 0x44};

// Scripts the full chunked ReadMemoryByAddress sweep the executor must
// perform over the plan's transfer region, filling every payload with `fill`.
void scriptFullRead(ScriptedCanFlashTransport& transport, const fastecu::flash::FlashPlan& plan,
                    bytes::Byte fill)
{
    const std::uint32_t start = plan.transfer_region().start;
    const std::uint32_t length = plan.transfer_region().length;
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
    auto plan = writePlan();

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

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    // The handshake completed; only the unimplemented write path stops it.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
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
    auto plan = writePlan();

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
    auto plan = writePlan();

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

} // namespace
