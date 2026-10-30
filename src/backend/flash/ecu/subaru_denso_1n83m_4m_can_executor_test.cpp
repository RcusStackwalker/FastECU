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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/ecu/testing/can_executor_run.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
#include "src/backend/flash/testing/faulting_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/guarded_attempt_expectations.h"
#include "src/backend/flash/ecu/testing/cancellation_blind_can_transport.h"
#include "src/backend/flash/testing/execute_with_cancellation_guard.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
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
using ::testing::_;
using testing::Contains;
using testing::Each;
using ::testing::Eq;
using testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;
using testing::Pair;

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_4m_can";
constexpr std::string_view kMcu = "N83M_4MB";

// kFlashBlocksN83M_4MB: [0] = {0x08F9C000, 0x10000}, [1] = {0x08FAC000,
// 0x3D3F00}, [2] = {0x0937FF00, 0x100}.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kBlockStart = 0x08FAC000;
constexpr std::uint32_t kBlockLength = 0x3D3F00;
constexpr std::size_t kImageSize = 0x3E4000;
constexpr std::uint32_t kPageSize = 0x100;

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
{
    return BuildSubaruDenso1n83m4mCanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruDenso1n83m4mCanPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
{
    return fastecu::flash::testing::HandBuiltCanPlan(
        operation, fastecu::flash::FlashFamily::kSubaruDenso1n83m4mCan, kProtocol, kMcu,
        fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}, kImageSize,
        SubaruDenso1n83m_4mCanPlan{0x7e0, 0x7e8, 500000, false, 0x10000, 0x100});
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
    ScriptDensoPreliminaries(t, 0xFF);
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

// Preliminaries plus the bench session request, then the security-access
// seed request registered as an expected write only -- its reply is left for
// the caller to queue, so the same script serves every "the next read fails"
// conformance test (fastecu::flash::testing::CanExecutorConformance)
// regardless of which failure mode (a transport error, a bare timeout, or an
// empty frame) belongs there.
//
// Unlike every sibling in this dialect, this family's read_memory dump-setup
// pair (the 0x34/0x35 exchanges) is NOT a safe place to stop: the dump-setup query
// treats a wrong OR an absent reply there as non-fatal (see
// ProceedsPastMalformedConnectAndDumpSetupResponses /
// ProceedsPastMalformedInCarProbeAndDumpSetup), so an empty-reply fault
// injected there would be swallowed rather than surfaced, silently defeating
// ReadReportsAnEmptyReplyAsTimeout. The security-access seed request
// (DensoSecurityAccess, FatalQuery-based) is the first exchange after the
// tolerant preliminaries that is fatal on all three fault kinds, so that is
// where this family's copy of the shared conformance script stops instead.
void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    ScriptDensoPreliminaries(t, 0xFF);
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
            const int absent_reads = jump_outcome == 3 ? 1 : 10;
            for (int attempt = 0; attempt < absent_reads; ++attempt)
            {
                t.QueueNoFrame();
            }
            t.QueueRead(Response({0x50, 0x62})); // late ACK; exhausted final receive is unchecked
        }
    } // one pre-loop read only
}

void ScriptInCarConnect(ScriptedCanFlashTransport& t, int jump_outcome = 0)
{
    const auto section = t.Section("in-car connect");
    ScriptDensoPreliminaries(t, 0x00);
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

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block). `encrypted` is the
// whole 0x3E4000 plan image, encrypted once, and indexed from kImageStart --
// so chunk 0 carries encrypted[0x10000..0x10100).
void ScriptReflashChunks(ScriptedCanFlashTransport& t, bytes::ByteView encrypted)
{
    const auto section = t.Section("reflash chunks");
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

// The write image and its wire encoding are the same for every write test, and
// encrypting a multi-megabyte image dominates their time, so each is built once
// per process, including when both plain and parameterized suites run.
class SubaruDenso1n83m_4mCanExecutorWrite : public testing::TestWithParam<int>
{
  protected:
    static void SetUpTestSuite()
    {
        // Plain and parameterized suites share these immutable fixtures.
        if (rom_.empty())
        {
            rom_ = fastecu::flash::testing::PatternedRom(kImageSize);
            wire_ = ToWire(rom_);
        }
    }

    static inline bytes::Bytes rom_;
    static inline bytes::Bytes wire_;
};

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

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDenso1n83m_4mCanExecutor>(*plan, transport);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    ASSERT_EQ(result->read_bytes->size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).subspan(0x10000, kBlockLength), test_bytes::BytesFilledWith(0x5A));
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
    ScriptDensoPreliminaries(transport, 0x00);
    ScriptInCarConnectTail(transport, 0x50, 0x02); // wrong subfunction, tolerated
    ScriptReadSetupWithMissingThenWrongReply(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDenso1n83m_4mCanExecutor>(*plan, transport);

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

    fastecu::flash::testing::ExpectPaddedRead<SubaruDenso1n83m_4mCanExecutor>(
        ReadPlan(), transport, kImageSize, 0x10000, kBlockLength, 0xA5,
        [](const auto& run, const auto&) { EXPECT_THAT(run.events.logs, testing::Not(IsEmpty())); });
    fastecu::flash::testing::ExpectSeedAndKeyReadTimeouts(transport);
}

TEST(SubaruDenso1n83m_4mCanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptInCarConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDenso1n83m_4mCanExecutor>(ReadPlan(), transport, kImageSize,
                                                                              0x10000, kBlockLength, 0x5A);
}

TEST_F(SubaruDenso1n83m_4mCanExecutorWrite, WriteErasesThenFlashesBlockOne)
{
    ScriptedCanFlashTransport transport;

    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    transport.QueueRead(Response({0x71, 0x01, 0x02}));
    ScriptReflashChunks(transport, wire_);
    ScriptCloseAndChecksum(transport);

    const auto plan = WritePlan(rom_);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] =
        ExecutorRun<SubaruDenso1n83m_4mCanExecutor, fastecu::RecordingClock>(*plan, transport);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    // Every 0xB6 chunk was matched byte-for-byte by the scripted exchanges
    // above; assert the indexing convention explicitly too, so a wrong image
    // base fails here with a readable message rather than as an "unexpected
    // write". reflash_block reads newdata[i + blockaddr - fblocks[0].start]
    // out of the caller's &data_array[0], which is the whole encrypted image
    // starting at fblocks[0].start -- so the first chunk of block 1 is
    // encrypted[0x10000..0x10100).
    const bytes::Bytes& encrypted = wire_;
    EXPECT_THAT(bytes::Bytes(encrypted.begin() + 0x10000, encrypted.begin() + 0x10000 + 256),
                test_bytes::BytesEq(ToWire(bytes::ByteView(rom_).subspan(0x10000, 256))));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait, the bench kernel jump's
    // inter-read settle, the settle after the erase command, and the settle
    // before the checksum-verify write. Asserted as a whole sequence rather
    // than by Contains so that dropping one -- as this port did with the
    // checksum-verify settle -- fails here instead of passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 50ms, 500ms, 100ms}));
}

// This family's own strictness boundary, distinct from both the dialect's
// mid-dump disconnect (pinned once, in the 1.5M suite) and the conformance suite's
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

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDenso1n83m_4mCanExecutor>(ReadPlan(), transport,
                                                                                 ErrorKind::kTimeout);
}

// The channel strips the CAN envelope, so a correct dump-setup reply is
// exactly the four payload bytes the tolerant check needs: both are accepted
// without logging either error.
TEST(SubaruDenso1n83m_4mCanExecutor, ExactFourByteDumpSetupRepliesAreAcceptedSilently)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDenso1n83m_4mCanExecutor>(*plan, transport);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kError, _))));
}

// The same boundary one step later: a transport failure at the 0x35 upload
// setup surfaces too, and the dump never starts.
TEST(SubaruDenso1n83m_4mCanExecutor, ReadTimeoutAtTheTolerantUploadSetupStillPropagates)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
                       Response({0x74, 0x20, 0x01, 0x05}));
    transport.Exchange(Request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    transport.QueueError(ErrorKind::kTimeout, "no reply");

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDenso1n83m_4mCanExecutor>(ReadPlan(), transport,
                                                                                 ErrorKind::kTimeout);
}

// And a failed send of the 0x34 dump setup is never mistaken for an absent
// reply: it surfaces before any read.
TEST(SubaruDenso1n83m_4mCanExecutor, SendFailureAtTheTolerantDumpSetupStillPropagates)
{
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    fastecu::flash::FaultingCanFlashTransport probe;
    ScriptBenchConnect(probe);
    probe.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    probe.QueueError(ErrorKind::kTimeout, "no reply");
    ASSERT_THAT(ExecutorRun<SubaruDenso1n83m_4mCanExecutor>(*plan, probe).result,
                fastecu::testing::IsErr(ErrorKind::kTimeout));

    fastecu::flash::FaultingCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.fail_write_at = probe.Writes();
    EXPECT_THAT(ExecutorRun<SubaruDenso1n83m_4mCanExecutor>(*plan, transport).result,
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("injected")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.Reads(), probe.Reads() - 1);
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Denso1n83m_4mCanTraits
    : fastecu::flash::testing::CanScriptForwarders<&::ReadPlan, &::HandBuiltPlan, &::ScriptBenchConnect,
                                                   &::ScriptReadSetup, &::ScriptFlashDump, &::ScriptStopCommand,
                                                   &::ScriptUpToFirstFatalRead>
{
    using Executor = SubaruDenso1n83m_4mCanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // ConnectBootloader's TolerantProbe exchanges read with this family's
    // short timeout (serial_read_short_timeout), like two of its three
    // siblings.
    static constexpr std::chrono::milliseconds kProbeTimeout{200};
    static constexpr int kProbeCount = 8; // seed and key now read at 2000ms
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDenso1n83m_4mCan, CanExecutorConformance, Denso1n83m_4mCanTraits);

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
        ScriptShortPageDumpAndStopRetries(transport, exhausted, kBlockStart, kBlockLength, kPageSize);
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ExpectShortPageRead<SubaruDenso1n83m_4mCanExecutor>(*plan, transport, exhausted,
                                                            {.block_length = kBlockLength,
                                                             .page_size = kPageSize,
                                                             .lead_pad = 0x10000,
                                                             .obk_active_log = "OBK is active",
                                                             .sleeps = std::vector<std::chrono::milliseconds>{}});
    }
}

// No acknowledgement is required after the bounded bench wait. A matching
// first frame is deliberately discarded in the two-read families.
TEST(SubaruDenso1n83m_4mCanExecutor, BenchJumpExhaustionPreservesReadCountAndTiming)
{
    ScriptedCanFlashTransport transport;
    ScriptDensoPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    transport.Exchange(Request({0x10, 0x42}));
    transport.QueueRead(Response({0x50, 0x42})); // discarded despite matching
    transport.QueueNoFrame();
    for (int attempt = 0; attempt < 49; ++attempt)
    {
        transport.QueueNoFrame();
    }
    transport.QueueRead(Response({0x50, 0x42})); // final receive is deliberately unchecked
    ScriptReadSetup(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "stop after tolerated jump");
    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(
        fastecu::flash::testing::ExecuteWithCancellationGuard(executor, *plan, transport, clock, cancellation, events),
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

// A whole-image script includes both edge chunks with address-sensitive data;
// N83M's nonzero image base must be subtracted exactly once.
TEST_P(SubaruDenso1n83m_4mCanExecutorWrite, CloseRetriesSettleAndChecksumOutcomes)
{
    const int outcome = GetParam();
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    transport.QueueRead(Response({0x71, 0x01, 0x02}));
    ScriptReflashChunks(transport, wire_);
    ScriptCloseRetries(transport, outcome == 0);
    ScriptChecksumOutcome(transport, outcome);
    const auto plan = WritePlan(rom_);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ExpectCloseOutcome<SubaruDenso1n83m_4mCanExecutor>(
        *plan, transport, outcome,
        {.sleeps = std::vector<std::chrono::milliseconds>{500ms, 50ms, 500ms, 100ms},
         .checksum_timeout = 500ms,
         .reread_timeout = 3000ms});
}

INSTANTIATE_TEST_SUITE_P(ChecksumOutcomes, SubaruDenso1n83m_4mCanExecutorWrite, testing::Range(0, 5),
                         testing::PrintToStringParamName());

TEST(SubaruDenso1n83m_4mCanExecutor, IdentityExchangeFailuresIncludingCancellationAreNonFatal)
{
    fastecu::flash::testing::ExpectIdentityFailuresAreNonFatal<SubaruDenso1n83m_4mCanExecutor>(
        &ReadPlan, {0xAA}, {200ms, 2000ms, 2000ms, 2000ms, 2000ms, 200ms});
}

TEST(SubaruDenso1n83m_4mCanExecutor, InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy)
{
    for (const int outcome : {0, 1, 2, 3})
    {
        SCOPED_TRACE(::testing::PrintToString(outcome));
        ScriptedCanFlashTransport transport;
        ScriptInCarConnect(transport, outcome);
        if (outcome != 2)
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
        EXPECT_THAT(fastecu::flash::testing::ExecuteWithCancellationGuard(executor, *plan, transport, clock,
                                                                          cancellation, events),
                    fastecu::testing::IsErr(outcome != 2 ? ErrorKind::kDisconnected : ErrorKind::kCancelled));
        EXPECT_TRUE(transport.ScriptConsumed());
        const auto& timeouts = transport.ReadTimeouts();
        const std::size_t count = outcome == 1 ? 11 : outcome == 3 ? 2 : 1;
        const std::size_t trailing_read_count = outcome != 2 ? 3 : 0;
        ASSERT_GE(timeouts.size(), count + trailing_read_count);
        const auto end =
            timeouts.end() - static_cast<std::vector<std::chrono::milliseconds>::difference_type>(trailing_read_count);
        EXPECT_THAT(std::vector<std::chrono::milliseconds>(
                        end - static_cast<std::vector<std::chrono::milliseconds>::difference_type>(count), end),
                    Each(200ms));
        std::vector<std::chrono::milliseconds> sleeps{500ms};
        if (outcome != 2)
        {
            sleeps.insert(sleeps.end(), outcome == 1 ? 10 : outcome == 3 ? 1 : 0, 100ms);
        }
        EXPECT_THAT(events.logs, testing::Contains(testing::Pair(
                                     outcome == 1 ? fastecu::LogLevel::kError : fastecu::LogLevel::kInfo,
                                     outcome == 1   ? "Kernel jump was not acknowledged; continuing, as legacy does"
                                     : outcome == 2 ? "Jump to onboad kernel"
                                                    : "Kernel jump acknowledged")));
        EXPECT_EQ(clock.sleep_calls, sleeps);
    }
}

TEST(SubaruDenso1n83m_4mCanExecutor, CancellationAfterTolerantSetupWriteRefusesBlindReply)
{
    fastecu::ManualCancellationToken cancellation;
    auto transport = std::make_unique<fastecu::flash::testing::CancellationBlindCanTransport>();
    auto *inner = transport.get();
    inner->Exchange(Request({0x10, 0x5F}), Response({0x50, 0x5F}));
    inner->Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}),
                    Response({0x74, 0x20, 0x01, 0x05}));
    int readsAtSetup = 0;
    inner->on_write = [&](bytes::ByteView data)
    {
        if (data.size() > 4 && data[4] == 0x34)
        {
            readsAtSetup = inner->read_attempts;
            cancellation.Cancel();
        }
    };
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto attempt = fastecu::flash::BindFlashAttempt(*plan, std::make_unique<SubaruDenso1n83m_4mCanExecutor>(),
                                                    std::move(transport));
    RecordingClock clock;
    RecordingEventSink events;
    EXPECT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
    const int setupReplyReadAttempts = inner->read_attempts - readsAtSetup;
    EXPECT_EQ(setupReplyReadAttempts, 0);
    EXPECT_TRUE(cancellation.Cancelled());
    EXPECT_FALSE(inner->ScriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, SelectedGuardIsUsedByBoundAttempt)
{
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    fastecu::flash::testing::ExpectCanGuardedAttempt<SubaruDenso1n83m_4mCanExecutor>(*plan);
}
} // namespace
