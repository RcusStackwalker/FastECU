#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for SubaruDenso1n83m_4mCanExecutor, the portable
// replacement for flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp.
//
// This family's defining property is tolerance: seven checks whose `return
// STATUS_ERROR` legacy commented out log and proceed where its 1N83M 1.5M and
// SH72531 siblings abort. The two `ProceedsPast...` tests below assert that
// positively -- between them they drive all seven -- and the mirror-image
// `NegativeResponseAtDumpSetupFails` cases in those two siblings' suites
// assert the opposite, so the difference cannot be normalized away in either
// direction without a test failing.
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
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
using fastecu::LogLevel;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::flash::BuildSubaruDenso1n83m4mCanPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDenso1n83m_4mCanExecutor;
using fastecu::flash::SubaruDenso1n83m_4mCanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::Contains;
using testing::Each;
using testing::IsEmpty;
using testing::Pair;

// Records every ctx.clock.sleep() argument so the executor's inter-exchange
// settles can be asserted as a sequence. Same shape as the recording clocks in
// subaru_denso_sh7055_02_executor_test.cpp and
// subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp: a
// FakeClock with one extra hook, so no fake or port changes shape.

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_4m_can";
constexpr std::string_view kMcu = "N83M_4MB";

// kFlashBlocksN83M_4MB: [0] = {0x08F9C000, 0x10000}, [1] = {0x08FAC000,
// 0x3D3F00}, [2] = {0x0937FF00, 0x100}.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kBlockStart = 0x08FAC000;
constexpr std::uint32_t kBlockLength = 0x3D3F00;
constexpr std::size_t kImageSize = 0x3E4000;
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
    return BuildSubaruDenso1n83m4mCanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruDenso1n83m4mCanPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_denso_1n83m_4m_can_plan, so
// a plan whose operation the builder itself would refuse can still reach the
// executor -- the only way to prove the executor's own
// validate_subaru_denso_1n83m_4m_can_plan call rejects it before any I/O.
fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::kSubaruDenso1n83m4mCan;
    fields.transport = fastecu::flash::TransportKind::kCanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kBlockStart, kBlockLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}};
    fields.image = bytes::Bytes(kImageSize, 0x00);
    fields.family_plan = SubaruDenso1n83m_4mCanPlan{0x7e0, 0x7e8, 500000, false, 0x10000, 0x100};
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

// The same preliminaries with every reply the family tolerates made bad: the
// four identity queries answered with NRCs, then the two checks whose `return
// STATUS_ERROR` is commented out -- the access-method probe and the branch
// selector -- answered with the wrong subfunction. None of these may stop the
// sequence. The selector's byte 3 still selects the branch, exactly as legacy
// reads it out of a reply legacy has already logged as wrong.
void ScriptPreliminariesWithNegativeIdReplies(ScriptedCanFlashTransport& t, bytes::Byte branch_byte)
{
    const auto section = t.Section("preliminaries with negative id replies");
    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));                          // OBK probe, miss
    t.Exchange(Request({0xAA}), Response({0x7F, 0xAA, 0x11}));                          // ECU ID
    t.Exchange(Request({0x09, 0x02}), Response({0x7F, 0x09, 0x11}));                    // VIN
    t.Exchange(Request({0x09, 0x04}), Response({0x7F, 0x09, 0x11}));                    // CAL ID
    t.Exchange(Request({0x09, 0x06}), Response({0x7F, 0x09, 0x11}));                    // CVN
    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x02}));                          // access method, tolerated
    t.Exchange(Request({0x22, 0x10, 0x1D}), Response({0x62, 0x11, 0x1D, branch_byte})); // branch selector, tolerated
}

// The bench arm after the preliminaries. The kernel jump reads TWICE before
// entering its wait loop -- an extra read this family's siblings do not
// perform -- so two matching replies are queued: a port that read only once
// would leave the second frame unconsumed and fail scriptConsumed().
void ScriptBenchConnectTail(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("bench connect tail");
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    t.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42})); // discarded
    t.QueueRead(Response({0x50, 0x42}));                       // the wait loop's seed
}

// The bench arm.
void ScriptBenchConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("bench connect");
    ScriptPreliminaries(t, 0xFF);
    ScriptBenchConnectTail(t);
}

// The 0x34/0x35 dump setup pair (legacy read_memory).
void ScriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
               Response({0x75, 0x20, 0x01, 0x01}));
}

// The same pair with the 0x34 reply's last header byte wrong and the 0x35
// reply absent altogether, both tolerated. Both must be logged and stepped
// over.
void ScriptReadSetupWithNegativeReplies(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup with negative replies");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x06}));
    t.Exchange(Request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.QueueNoFrame();
}

// The mirror of the above: the 0x34 reply absent and the 0x35 reply's service
// byte wrong, both tolerated.
void ScriptReadSetupWithMissingThenWrongReply(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup with missing then wrong reply");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.QueueNoFrame();
    t.Exchange(Request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
               Response({0x7F, 0x35, 0x31}));
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

// Preliminaries plus the bench session request, then the security-access
// seed request registered as an expected write only -- its reply is left for
// the caller to queue, so the same script serves every "the next read fails"
// conformance test (fastecu::flash::testing::CanExecutorConformance)
// regardless of which failure mode (a transport error, a bare timeout, or an
// empty frame) belongs there.
//
// Unlike every sibling in this cluster, this family's read_memory dump-setup
// pair (the 0x34/0x35 exchanges) is NOT a safe place to stop: tolerant_setup
// treats a wrong OR an absent reply there as non-fatal (see
// ProceedsPastMalformedConnectAndDumpSetupResponses /
// ProceedsPastMalformedInCarProbeAndDumpSetup), so an empty-reply fault
// injected there would be swallowed rather than surfaced, silently defeating
// ReadReportsAnEmptyReplyAsTimeout. The security-access seed request
// (security_access(), fatal_query-based) is the first exchange after the
// tolerant preliminaries that is fatal on all three fault kinds, so that is
// where this family's copy of the shared conformance script stops instead.
void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    ScriptPreliminaries(t, 0xFF);
    const auto section = t.Section("bench connect tail (first fatal request only)");
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}));
}

// The in-car arm. The ten fire-and-forget replies are deliberately given
// arbitration ids other than 0x7E8 wherever the addressed module would answer
// on its own id: legacy reads whichever frame arrives next without checking
// the id, and this pins that the port does not add a check legacy lacks.
// `probeService`/`probeSub` parameterize the probe so the tolerance can be
// driven.
void ScriptInCarConnectTail(ScriptedCanFlashTransport& t, bytes::Byte probe_service, bytes::Byte probe_sub)
{
    const auto section = t.Section("in-car connect tail");
    t.Exchange(Request({0x10, 0x5F}), Response({probe_service, probe_sub})); // tolerated

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
    t.Exchange(Request({0x10, 0x62}), Response({0x50, 0x62}));                   // one pre-loop read only
}

void ScriptInCarConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("in-car connect");
    ScriptPreliminaries(t, 0x00);
    ScriptInCarConnectTail(t, 0x50, 0x01);
}

// erase_memory's setup PDU plus its erase trigger; the trigger's answer is
// consumed by the re-read loop, not by a paired read.
void ScriptEraseMemory(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("erase memory");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block). `rom` is the
// whole 0x3E4000 plan image, encrypted once, and indexed from kImageStart --
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

TEST(SubaruDenso1n83m_4mCanExecutor, ProceedsPastMalformedConnectAndDumpSetupResponses)
{
    // The tolerance this family exists to preserve. A happy-path-only suite
    // would pass against a wrongly strict port, so this drives four of the
    // seven commented-out returns -- two in connect_bootloader and two in
    // read_memory -- and requires a complete, correctly sized ROM out the far
    // end anyway.
    ScriptedCanFlashTransport transport;
    ScriptPreliminariesWithNegativeIdReplies(transport, 0xFF);
    ScriptBenchConnectTail(transport);
    ScriptReadSetupWithNegativeReplies(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).subspan(0x10000, kBlockLength), Each(0x5A));
    EXPECT_TRUE(transport.ScriptConsumed());
    // Legacy's own wording for the absent-reply branch it then steps over.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST(SubaruDenso1n83m_4mCanExecutor, ProceedsPastMalformedInCarProbeAndDumpSetup)
{
    // The remaining three tolerated returns: the in-car access-method probe,
    // and the other halves of the two dump-setup checks. The dump itself is
    // cut short with a transport error, so reaching the 0xB7 sweep at all is
    // the proof the setup checks did not abort.
    ScriptedCanFlashTransport transport;
    ScriptPreliminaries(transport, 0x00);
    ScriptInCarConnectTail(transport, 0x50, 0x02); // wrong subfunction, tolerated
    ScriptReadSetupWithMissingThenWrongReply(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST(SubaruDenso1n83m_4mCanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

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

TEST(SubaruDenso1n83m_4mCanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptInCarConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

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

TEST(SubaruDenso1n83m_4mCanExecutor, WriteErasesThenFlashesBlockOne)
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
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    // Every 0xB6 chunk was matched byte-for-byte by the scripted exchanges
    // above; assert the indexing convention explicitly too, so a wrong image
    // base fails here with a readable message rather than as an "unexpected
    // write". reflash_block reads newdata[i + blockaddr - fblocks[0].start]
    // out of the caller's &data_array[0], which is the whole encrypted image
    // starting at fblocks[0].start -- so the first chunk of block 1 is
    // encrypted[0x10000..0x10100).
    const bytes::Bytes encrypted = ToWire(rom);
    EXPECT_THAT(bytes::Bytes(encrypted.begin() + 0x10000, encrypted.begin() + 0x10000 + 256),
                test_bytes::BytesEq(ToWire(bytes::ByteView(rom).subspan(0x10000, 256))));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait, the bench kernel jump's
    // inter-read settle, the settle after the erase command, and the settle
    // before the checksum-verify write. Asserted as a whole sequence rather
    // than by Contains so that dropping one -- as this port did with the
    // checksum-verify settle -- fails here instead of passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 50ms, 500ms, 100ms}));
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
TEST(SubaruDenso1n83m_4mCanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
}

// This family's own strictness boundary, distinct from both
// ReadDisconnectMidDumpLoopPropagates above and the conformance suite's
// ReadTimeoutPropagates (can_executor_conformance.h, which now faults at the
// seed request -- see scriptUpToFirstFatalRead's comment): read_memory's
// 0x34/0x35 dump-setup pair tolerates a wrong OR an absent reply
// (ProceedsPastMalformedConnectAndDumpSetupResponses proves that positively),
// but a genuine transport-level failure at that exact same point still has
// to surface, because legacy's read_serial_data has no error channel at all
// and a broken bus is not the same thing as an empty reply. Keeping both the
// tolerant case and this one is what stops the difference being normalized
// away in either direction.
TEST(SubaruDenso1n83m_4mCanExecutor, ReadTimeoutAtTheTolerantDumpSetupStillPropagates)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    transport.QueueError(ErrorKind::kTimeout, "no reply");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, NegativeResponseDuringConnectFails)
{
    // Tolerance is not blanket: the seed request keeps its live `return
    // STATUS_ERROR`, so a negative response there must abort even in this
    // family.
    ScriptedCanFlashTransport transport;
    ScriptPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x7F, 0x27, 0x35}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, EmptyBranchSelectorReplyFails)
{
    // The wrong-reply return is commented out in legacy but the absent-reply
    // one is not: a *wrong* branch-selector reply is tolerated, an *absent*
    // one still returns STATUS_ERROR. This pins that half of the check, which
    // the tolerance must not swallow.
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
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, EraseRetryExhaustionFails)
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
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    // A second trigger would have hit the end of the script and surfaced as an
    // Internal error rather than BadResponse; the failure is the erase's own.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Flash area erase failed")));
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Denso1n83m_4mCanTraits
{
    using Executor = SubaruDenso1n83m_4mCanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // connect_bootloader's tolerant_probe exchanges read with this family's
    // short timeout (serial_read_short_timeout), like two of its three
    // siblings.
    static constexpr std::chrono::milliseconds kProbeTimeout{200};
    static constexpr int kProbeCount = 8; // seed and key now read at 2000ms

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

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDenso1n83m_4mCan, CanExecutorConformance, Denso1n83m_4mCanTraits);

} // namespace
