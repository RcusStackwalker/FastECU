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
void ScriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branch_byte, int probe_case = 0)
{
    const auto section = t.Section("preliminaries");
    t.Exchange(Request({0x10, 0x5F}));
    if (probe_case == 1)
    {
        t.QueueNoFrame();
    }
    else if (probe_case == 2)
    {
        t.QueueRead(Response({}));
    }
    else
    {
        t.QueueRead(Response({0x50, 0x01}));
    }
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
void ScriptBenchConnect(ScriptedCanFlashTransport& t, int probe_case = 0)
{
    const auto section = t.Section("bench connect");
    ScriptPreliminaries(t, 0xFF, probe_case);
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
void ScriptInCarConnectTail(ScriptedCanFlashTransport& t, bytes::Byte probe_service, bytes::Byte probe_sub,
                            int jump_outcome = 0)
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
    if (jump_outcome == 0)
    {
        t.Exchange(Request({0x10, 0x62}), Response({0x50, 0x62}));
    }
    else
    {
        t.Exchange(Request({0x10, 0x62}));
        if (jump_outcome == 2)
        {
            t.QueueError(ErrorKind::kCancelled, "jump cancelled");
        }
        else
        {
            for (int attempt = 0; attempt < 11; ++attempt)
            {
                t.QueueNoFrame();
            }
        }
    } // one pre-loop read only
}

void ScriptInCarConnect(ScriptedCanFlashTransport& t, int jump_outcome = 0)
{
    const auto section = t.Section("in-car connect");
    ScriptPreliminaries(t, 0x00);
    ScriptInCarConnectTail(t, 0x50, 0x01, jump_outcome);
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
        // Frozen complete requests for WriteRom's address-sensitive image, generated
        // once with an independent integer Feistel implementation. These boundary
        // oracles never invoke ComposeBe, ToWire or production crypto at runtime.
        if (offset == 0)
        {
            t.Exchange(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0x08, 0xFA, 0xC0, 0x00, 0x43, 0xE6, 0x15, 0x9E, 0x65,
                                    0x55, 0x00, 0xD6, 0xE5, 0x45, 0xC6, 0xEE, 0x01, 0x26, 0x5F, 0xA8, 0x81, 0x38, 0x9F,
                                    0xE2, 0xF9, 0xD3, 0xCA, 0x36, 0xD8, 0x79, 0xC6, 0x0F, 0x31, 0x84, 0xF9, 0x73, 0x21,
                                    0xB3, 0x83, 0x27, 0xD3, 0xE6, 0x24, 0xFE, 0xFE, 0xF7, 0x73, 0x6C, 0x36, 0xA3, 0x19,
                                    0x21, 0xC3, 0x72, 0xC3, 0xF9, 0x0B, 0xC2, 0x5A, 0x23, 0xF6, 0xC6, 0xCA, 0x43, 0x8F,
                                    0x78, 0x8B, 0x3D, 0xEA, 0x8F, 0x02, 0xB3, 0x67, 0x23, 0x4C, 0xFD, 0x20, 0x9D, 0x89,
                                    0x1E, 0x65, 0x7A, 0x8C, 0x47, 0x00, 0xF6, 0x8B, 0x7B, 0xF1, 0x2D, 0x74, 0x88, 0xA4,
                                    0x69, 0x45, 0x34, 0x06, 0x7B, 0x43, 0x9A, 0x9D, 0x46, 0x3A, 0x3A, 0xE4, 0x9C, 0xEF,
                                    0x47, 0xFB, 0xE5, 0xCD, 0xF2, 0x94, 0xA0, 0x94, 0xA7, 0xF2, 0xFA, 0xE8, 0x84, 0x25,
                                    0x33, 0x03, 0xD4, 0x65, 0xEE, 0x37, 0x94, 0x4D, 0xD5, 0x43, 0x56, 0x8C, 0xB4, 0x33,
                                    0x1B, 0xB3, 0x7F, 0xA9, 0x3A, 0x00, 0xBD, 0x32, 0x08, 0xC4, 0x93, 0x94, 0x02, 0x09,
                                    0xB9, 0x8B, 0x60, 0x2B, 0x34, 0xE7, 0xCF, 0xDB, 0x69, 0xFF, 0xAD, 0x45, 0x9A, 0x98,
                                    0x25, 0x66, 0x28, 0xD3, 0x6A, 0x50, 0x87, 0xF4, 0x76, 0x9F, 0xB9, 0xD3, 0xA5, 0xBB,
                                    0x37, 0x94, 0x70, 0xCB, 0x5D, 0xCB, 0x94, 0x52, 0x3A, 0x4E, 0x16, 0x3C, 0x03, 0x51,
                                    0x00, 0x74, 0x7D, 0x34, 0xDF, 0xFD, 0x9C, 0xC2, 0x89, 0x51, 0xA6, 0x4A, 0xE4, 0x8A,
                                    0xEE, 0xFB, 0xF7, 0xF4, 0xE2, 0x5D, 0x9E, 0x74, 0xF8, 0xC2, 0xEC, 0x49, 0x16, 0xC0,
                                    0xBB, 0xC8, 0xC0, 0x76, 0xE1, 0x8C, 0xEC, 0x43, 0x50, 0x98, 0xAC, 0xDF, 0xA3, 0x6E,
                                    0x19, 0xF2, 0x3F, 0x81, 0x68, 0xD4, 0xAE, 0x7A, 0xBE, 0xB7, 0xDA, 0x72, 0xB1, 0xFB,
                                    0xD9, 0x43, 0xC4, 0x57, 0x5F, 0x7B, 0x23, 0x9B, 0x36, 0x73, 0x41, 0xEA, 0xED},
                       Response({0xF6}));
        }
        else if (offset == kBlockLength - 256)
        {
            t.Exchange(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0x09, 0x37, 0xFE, 0x00, 0xE0, 0xD3, 0x85, 0x2B, 0x95,
                                    0xDE, 0x83, 0x5A, 0xAE, 0x20, 0x2D, 0xB2, 0x72, 0x5A, 0x3F, 0x6D, 0xB0, 0x2D, 0xCE,
                                    0x6A, 0x13, 0xCD, 0x0F, 0x86, 0x20, 0x47, 0x6A, 0x1B, 0x80, 0xAA, 0x24, 0x85, 0x15,
                                    0xD9, 0xA5, 0x80, 0x49, 0xE3, 0x7D, 0x03, 0x05, 0x59, 0xDA, 0x6D, 0x59, 0x52, 0x6E,
                                    0xFF, 0xD5, 0x19, 0x1C, 0x20, 0x2C, 0xCC, 0x48, 0x1F, 0xBA, 0x62, 0xAF, 0xA6, 0xE0,
                                    0x01, 0xEB, 0x90, 0x1E, 0x88, 0xF9, 0x1E, 0xAE, 0xBC, 0x5C, 0x8A, 0x90, 0x71, 0xE8,
                                    0x4C, 0x1B, 0xDD, 0x70, 0x62, 0xBA, 0x4F, 0x12, 0xA1, 0x12, 0xEC, 0xD5, 0xED, 0xD5,
                                    0xEF, 0x20, 0xB1, 0x1D, 0xC5, 0x0C, 0xCC, 0xF9, 0x2E, 0x10, 0xBD, 0xCB, 0xBF, 0xB5,
                                    0x49, 0x52, 0x26, 0x4B, 0xEB, 0x27, 0xDA, 0xD0, 0x9B, 0xFF, 0x17, 0x7F, 0xE8, 0x88,
                                    0xF9, 0x81, 0x41, 0xA7, 0x94, 0x8B, 0xE2, 0x1A, 0xEB, 0x15, 0x40, 0xA9, 0x8E, 0x7B,
                                    0x6D, 0xF9, 0x1E, 0xCA, 0xAC, 0xF3, 0xDE, 0x61, 0x5E, 0xA5, 0xEB, 0x06, 0xFF, 0x59,
                                    0x0B, 0xBB, 0x01, 0xFF, 0x87, 0x61, 0xC3, 0x20, 0x76, 0xD1, 0x38, 0x63, 0xD7, 0x6E,
                                    0x2E, 0x17, 0xC5, 0x08, 0x7D, 0x22, 0x45, 0xCD, 0x38, 0xFB, 0x0B, 0x6D, 0x15, 0x56,
                                    0xDC, 0x3E, 0x29, 0x5B, 0x10, 0xA2, 0x31, 0xB7, 0x50, 0xE0, 0xE2, 0x22, 0x5C, 0x00,
                                    0xD3, 0x41, 0x6D, 0x80, 0x71, 0x38, 0x53, 0xD1, 0xBD, 0xAF, 0x67, 0xEC, 0xB0, 0xC3,
                                    0x16, 0xD2, 0x3E, 0x48, 0x48, 0x93, 0xDA, 0x50, 0xE9, 0x51, 0x1D, 0xCB, 0x75, 0x20,
                                    0x00, 0x24, 0xF0, 0x77, 0x10, 0x15, 0x7D, 0x2B, 0x99, 0xE7, 0x5A, 0x2C, 0x7A, 0x1E,
                                    0xC8, 0x83, 0x8C, 0xCA, 0xB7, 0x2F, 0x14, 0xA5, 0xC7, 0x4F, 0x5C, 0x74, 0x9A, 0xD3,
                                    0xFC, 0x0C, 0x98, 0x26, 0x6D, 0x2A, 0x4A, 0x9E, 0x1C, 0xC5, 0xFE, 0x4B, 0x39},
                       Response({0xF6}));
        }
        else
        {
            t.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB6), addr,
                                                bytes::ByteView(encrypted).subspan(addr - kImageStart, 256))),
                       Response({0xF6}));
        }
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
        rom[i] = static_cast<bytes::Byte>((i ^ (i >> 8) ^ (i >> 16)) & 0xFF);
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

// Independent family scripts: characterize the current boundaries, including
// permissiveness, without invoking a new production helper or normalizing it.
TEST(SubaruDenso1n83m_4mCanExecutor, ObkMismatchAbsentAndEmptyAreMisses)
{
    for (int probe_case = 0; probe_case < 3; ++probe_case)
    {
        SCOPED_TRACE(probe_case);
        ScriptedCanFlashTransport transport;
        ScriptBenchConnect(transport, probe_case);
        ScriptReadSetup(transport);
        transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
        transport.QueueError(ErrorKind::kDisconnected, "stop after successful connection");
        FakeClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kDisconnected));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_THAT(events.logs,
                    testing::Contains(testing::Pair(fastecu::LogLevel::kInfo, "OBK not active, initialising ECU...")));
        EXPECT_EQ(transport.ReadTimeouts().front(), 200ms);
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, ObkErrorsAndCancellationPropagateBeforeInitialization)
{
    for (const auto kind : {ErrorKind::kDisconnected, ErrorKind::kCancelled, ErrorKind::kBadResponse})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        ScriptedCanFlashTransport transport;
        transport.Exchange(Request({0x10, 0x5F}));
        if (kind == ErrorKind::kBadResponse)
        {
            transport.QueueRead(bytes::Bytes{}); // malformed envelope, unlike an empty PDU
        }
        else
        {
            transport.QueueError(kind, "probe failure");
        }
        FakeClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsErr(kind));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_THAT(events.logs, testing::Not(testing::Contains(
                                     testing::Pair(fastecu::LogLevel::kInfo, "OBK not active, initialising ECU..."))));
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort)
{
    for (const bool exhausted : {false, true})
    {
        SCOPED_TRACE(exhausted);
        ScriptedCanFlashTransport transport;
        // Prefix hit (extra byte deliberately allowed) bypasses all identity,
        // access-method, security and jump exchanges.
        transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x5F, 0x99}));
        ScriptReadSetup(transport);
        auto wire = ToWire(bytes::Bytes{0x12, 0x34, 0x56, 0x78});
        wire.insert(wire.end(), {0xAA, 0xBB, 0xCC});
        auto reply = Response({0xF7});
        reply.insert(reply.end(), wire.begin(), wire.end());
        for (std::uint32_t offset = 0; offset < kBlockLength; offset += kPageSize)
        {
            transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart + offset)), reply);
        }
        // Six sends maximum. All failed kinds are tolerated, even a cancelled
        // exchange. A positive sixth reply breaks without logging; exhaustion also
        // returns the padded image. No reply length check is added here.
        transport.Exchange(Request({0x37}));
        transport.QueueNoFrame();
        transport.Exchange(Request({0x37}), bytes::Bytes{});
        transport.Exchange(Request({0x37}), Response({0x7F, 0x37, 0x22}));
        transport.Exchange(Request({0x37}));
        transport.QueueError(ErrorKind::kDisconnected, "stop disconnected");
        transport.Exchange(Request({0x37}));
        transport.QueueError(ErrorKind::kCancelled, "stop cancelled");
        transport.Exchange(Request({0x37}));
        if (exhausted)
        {
            transport.QueueError(ErrorKind::kTimeout, "stop exhausted");
        }
        else
        {
            transport.QueueRead(Response({0x77}));
        }
        RecordingClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        const auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_THAT(result, fastecu::testing::IsOk());
        ASSERT_TRUE(result->read_bytes.has_value());
        constexpr std::size_t kPages = kBlockLength / kPageSize;
        constexpr std::size_t kLead = 0x10000;
        ASSERT_EQ(result->read_bytes->size(), kLead + kPages * 4 + 0x100);
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(kLead), Each(0xFF));
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(0x100), Each(0xFF));
        for (std::size_t page = 0; page < kPages; ++page)
        {
            SCOPED_TRACE(page);
            EXPECT_THAT(bytes::ByteView(*result->read_bytes).subspan(kLead + page * 4, 4),
                        testing::ElementsAre(0x12, 0x34, 0x56, 0x78));
        }
        std::vector<int> read_progress;
        for (const auto& event : events.phase_progress_calls)
        {
            if (event.phase_name == "Read ROM")
            {
                read_progress.push_back(event.done);
            }
        }
        ASSERT_EQ(read_progress.size(), kPages + 2);
        EXPECT_EQ(read_progress.front(), 0);
        for (std::size_t page = 1; page <= kPages; ++page)
        {
            SCOPED_TRACE(page);
            EXPECT_EQ(read_progress[page], static_cast<int>(page == kPages ? kBlockLength - 1 : page * kPageSize));
        }
        EXPECT_EQ(read_progress.back(), static_cast<int>(kBlockLength));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::kInfo, "OBK is active")));
        EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{}));
        EXPECT_EQ(transport.ReadTimeouts().back(), 500ms);
        EXPECT_THAT(events.logs, testing::Not(testing::Contains(
                                     testing::Pair(fastecu::LogLevel::kInfo, "Stop request response: 77 "))));
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, PageAbsentEmptyAndCancelledFailWithoutStopOrProgress)
{
    for (int response_case = 0; response_case < 3; ++response_case)
    {
        SCOPED_TRACE(response_case);
        ScriptedCanFlashTransport transport;
        transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x5F}));
        ScriptReadSetup(transport);
        transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
        if (response_case == 0)
        {
            transport.QueueNoFrame();
        }
        else if (response_case == 1)
        {
            transport.QueueRead(bytes::Bytes{});
        }
        else
        {
            transport.QueueError(ErrorKind::kCancelled, "page cancelled");
        }
        FakeClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        const auto kind = response_case == 0   ? ErrorKind::kTimeout
                          : response_case == 1 ? ErrorKind::kBadResponse
                                               : ErrorKind::kCancelled;
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsErr(kind));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().done, 0);
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, CancellationAfterPageFailsButCancellationAtStopIsSwallowed)
{
    for (const bool at_stop : {false, true})
    {
        SCOPED_TRACE(at_stop);
        ScriptedCanFlashTransport transport;
        transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x5F}));
        ScriptReadSetup(transport);
        ScriptFlashDump(transport, kBlockStart, at_stop ? kBlockLength : kPageSize, kPageSize, 0xA5);
        fastecu::ManualCancellationToken cancellation;
        class CancellingEvents final : public RecordingEventSink
        {
          public:
            CancellingEvents(fastecu::ManualCancellationToken& token, bool stop) : token_(token), stop_(stop)
            {
            }
            void Log(fastecu::LogLevel level, std::string_view text) override
            {
                RecordingEventSink::Log(level, text);
                if (stop_ && text == "Sending stop command")
                {
                    token_.Cancel();
                }
            }
            void PhaseProgress(const fastecu::PhaseProgressEvent& event) override
            {
                RecordingEventSink::PhaseProgress(event);
                if (!stop_ && event.phase_name == "Read ROM" && event.done == static_cast<int>(kPageSize))
                {
                    token_.Cancel();
                }
            }

          private:
            fastecu::ManualCancellationToken& token_;
            bool stop_;
        } events(cancellation, at_stop);
        FakeClock clock;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        const auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        if (at_stop)
        {
            EXPECT_THAT(result, fastecu::testing::IsOk());
            EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(kBlockLength));
        }
        else
        {
            EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
            EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(kPageSize));
        }
        EXPECT_TRUE(cancellation.Cancelled());
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

// No acknowledgement is required after the bounded bench wait. A matching
// first frame is deliberately discarded in the two-read families.
TEST(SubaruDenso1n83m_4mCanExecutor, BenchJumpExhaustionPreservesReadCountAndTiming)
{
    ScriptedCanFlashTransport transport;
    ScriptPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    transport.Exchange(Request({0x10, 0x42}));
    transport.QueueRead(Response({0x50, 0x42})); // discarded despite matching
    transport.QueueNoFrame();
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        transport.QueueNoFrame();
    }
    ScriptReadSetup(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "stop after tolerated jump");
    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                testing::Contains(testing::Pair(fastecu::LogLevel::kError,
                                                "Kernel jump was not acknowledged; continuing, as legacy does")));
    std::vector<std::chrono::milliseconds> sleeps{500ms, 50ms};
    sleeps.insert(sleeps.end(), 50, 100ms);
    EXPECT_EQ(clock.sleep_calls, sleeps);
    const auto& timeouts = transport.ReadTimeouts();
    ASSERT_GE(timeouts.size(), 65U);
    EXPECT_EQ(timeouts[10], 200ms);
    EXPECT_THAT(std::vector<std::chrono::milliseconds>(timeouts.begin() + 11, timeouts.begin() + 62), Each(200ms));
}

void ScriptCloseRetries(ScriptedCanFlashTransport& transport, bool acknowledged)
{
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        transport.Exchange(Request({0x37}));
        if (attempt == 5 && acknowledged)
        {
            transport.QueueRead(Response({0x77}));
        }
        else if (attempt == 1)
        {
            transport.QueueError(ErrorKind::kDisconnected, "close ignored");
        }
        else if (attempt == 2)
        {
            transport.QueueRead(Response({0x7F, 0x37, 0x31}));
        }
        else if (attempt == 3)
        {
            transport.QueueError(ErrorKind::kCancelled, "close cancellation ignored");
        }
        else
        {
            transport.QueueNoFrame();
        }
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, CancellationAtCloseSettlePreventsChecksum)
{
    ScriptedCanFlashTransport transport;
    const auto rom = WriteRom();
    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    transport.QueueRead(Response({0x71, 0x01, 0x02}));
    ScriptReflashChunks(transport, rom);
    ScriptCloseRetries(transport, true);
    fastecu::ManualCancellationToken cancellation;
    class CancellingCloseEvents final : public RecordingEventSink
    {
      public:
        explicit CancellingCloseEvents(fastecu::ManualCancellationToken& token) : token_(token)
        {
        }
        void Log(fastecu::LogLevel level, std::string_view message) override
        {
            RecordingEventSink::Log(level, message);
            if (message == "Closed succesfully: 77 ")
            {
                token_.Cancel();
            }
        }

      private:
        fastecu::ManualCancellationToken& token_;
    } events(cancellation);
    RecordingClock clock;
    SubaruDenso1n83m_4mCanExecutor executor;
    const auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));

    EXPECT_TRUE(transport.ScriptConsumed());
    ASSERT_FALSE(clock.sleep_calls.empty());
    EXPECT_EQ(clock.sleep_calls.back(), 100ms);
    EXPECT_THAT(events.logs,
                ::testing::Not(::testing::Contains(::testing::Pair(fastecu::LogLevel::kInfo, "Verifying checksum"))));
}

// A whole-image script includes both edge chunks with address-sensitive data;
// N83M's nonzero image base must be subtracted exactly once.
TEST(SubaruDenso1n83m_4mCanExecutor, CloseRetriesSettleAndChecksumOutcomes)
{
    for (int outcome = 0; outcome < 5; ++outcome)
    {
        SCOPED_TRACE(outcome);
        ScriptedCanFlashTransport transport;
        const auto rom = WriteRom();
        ScriptBenchConnect(transport);
        ScriptEraseMemory(transport);
        transport.QueueRead(Response({0x71, 0x01, 0x02}));
        ScriptReflashChunks(transport, rom);
        ScriptCloseRetries(transport, outcome == 0);
        transport.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}));
        if (outcome == 1)
        {
            transport.QueueRead(Response({0x7F, 0x31, 0x78}));
            transport.QueueRead(Response({0x71, 0x01, 0x02}));
        }
        else if (outcome == 2)
        {
            transport.QueueNoFrame();
        }
        else if (outcome == 3)
        {
            transport.QueueError(ErrorKind::kDisconnected, "checksum fails");
        }
        else if (outcome == 4)
        {
            transport.QueueRead(Response({0x7F, 0x31, 0x78}));
            transport.QueueNoFrame();
        }
        else
        {
            transport.QueueRead(Response({0x71, 0x01, 0x02}));
        }
        RecordingClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = WritePlan(rom);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        const auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        if (outcome < 2)
        {
            EXPECT_THAT(result, fastecu::testing::IsOk());
        }
        else
        {
            EXPECT_THAT(result, fastecu::testing::IsErr(outcome == 3 ? ErrorKind::kDisconnected : ErrorKind::kTimeout));
        }
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 50ms, 500ms, 100ms}));
        const auto& timeouts = transport.ReadTimeouts();
        const bool reread = outcome == 1 || outcome == 4;
        ASSERT_GE(timeouts.size(), 8U);
        const std::size_t checksum_index = timeouts.size() - (reread ? 2 : 1);
        EXPECT_EQ(timeouts[checksum_index], 500ms);
        if (reread)
        {
            EXPECT_EQ(timeouts.back(), 3000ms);
        }
        EXPECT_THAT(std::vector<std::chrono::milliseconds>(
                        timeouts.begin() +
                            static_cast<std::vector<std::chrono::milliseconds>::difference_type>(checksum_index) - 6,
                        timeouts.begin() +
                            static_cast<std::vector<std::chrono::milliseconds>::difference_type>(checksum_index)),
                    Each(200ms));
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, IdentityExchangeFailuresIncludingCancellationAreNonFatal)
{
    for (const auto error : {ErrorKind::kDisconnected, ErrorKind::kCancelled})
    {
        ScriptedCanFlashTransport transport;
        transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
        transport.Exchange(Request({0xAA}));
        transport.QueueError(error, "identity failure");
        for (const bytes::Byte subfunction : {0x02, 0x04, 0x06})
        {
            transport.Exchange(Request({0x09, subfunction}));
            transport.QueueError(error, "identity failure");
        }
        transport.Exchange(Request({0x10, 0x5F}));
        transport.QueueError(ErrorKind::kTimeout, "stop after identities");
        RecordingClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kTimeout));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(transport.ReadTimeouts(),
                  (std::vector<std::chrono::milliseconds>{200ms, 2000ms, 2000ms, 2000ms, 2000ms, 200ms}));
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy)
{
    for (const int outcome : {1, 2})
    {
        ScriptedCanFlashTransport transport;
        ScriptInCarConnect(transport, outcome);
        if (outcome == 1)
        {
            ScriptReadSetup(transport);
            transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
            transport.QueueError(ErrorKind::kDisconnected, "stop after jump");
        }
        RecordingClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        SubaruDenso1n83m_4mCanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(outcome == 1 ? ErrorKind::kDisconnected : ErrorKind::kCancelled));
        EXPECT_TRUE(transport.ScriptConsumed());
        const auto& timeouts = transport.ReadTimeouts();
        const std::size_t count = outcome == 1 ? 11 : 1;
        const std::size_t trailing_read_count = outcome == 1 ? 3 : 0;
        ASSERT_GE(timeouts.size(), count + trailing_read_count);
        const auto end =
            timeouts.end() - static_cast<std::vector<std::chrono::milliseconds>::difference_type>(trailing_read_count);
        EXPECT_THAT(std::vector<std::chrono::milliseconds>(
                        end - static_cast<std::vector<std::chrono::milliseconds>::difference_type>(count), end),
                    Each(200ms));
        std::vector<std::chrono::milliseconds> sleeps{500ms};
        if (outcome == 1)
        {
            sleeps.insert(sleeps.end(), 10, 100ms);
        }
        EXPECT_EQ(clock.sleep_calls, sleeps);
    }
}
} // namespace
