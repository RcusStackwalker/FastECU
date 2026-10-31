#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for SubaruDenso1n83m_1_5mCanExecutor, the portable
// replacement for flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp.
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/ecu/testing/can_executor_run.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::flash::BuildSubaruDenso1n83m15mCanPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDenso1n83m_1_5mCanExecutor;
using fastecu::flash::SubaruDenso1n83m_1_5mCanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using ::testing::_;
using testing::Each;
using ::testing::Eq;
using testing::IsEmpty;
using ::testing::Optional;

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_1_5m_can";
constexpr std::string_view kMcu = "N83M_1_5MB";

// kFlashBlocksN83M_1_5MB: [0] = {0x08F9C000, 0x10000}, [1] = {0x08FAC000,
// 0x173F00}, [2] = {0x0911FF00, 0x100}.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kBlockStart = 0x08FAC000;
constexpr std::uint32_t kBlockLength = 0x173F00;
constexpr std::size_t kImageSize = 0x184000;
constexpr std::uint32_t kPageSize = 0x100;

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
{
    return BuildSubaruDenso1n83m15mCanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruDenso1n83m15mCanPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
{
    return fastecu::flash::testing::HandBuiltCanPlan(
        operation, kProtocol, kMcu, fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}, kImageSize,
        SubaruDenso1n83m_1_5mCanPlan{0x7e0, 0x7e8, 500000, false, 0x10000, 0x100});
}

// The bench arm.
void ScriptBenchConnect(ScriptedCanFlashTransport& t, int probe_case = 0)
{
    const auto section = t.Section("bench connect");
    ScriptDensoPreliminaries(t, 0xFF, probe_case);
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    t.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory).
void ScriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               Response({0x75, 0x20, 0x01, 0x01}));
}

// Connect plus read_memory's 0x34 dump-setup request, registered as an
// expected write only -- its reply is left for the caller to queue, so the
// same script serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there.
void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    ScriptBenchConnect(t);
    const auto section = t.Section("read setup (first request only)");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}));
}

// The in-car arm. The ten fire-and-forget replies are deliberately given
// arbitration ids other than 0x7E8 wherever the addressed module would answer
// on its own id: legacy reads whichever frame arrives next without checking
// the id, and this pins that the port does not add a check legacy lacks.
void ScriptInCarConnect(ScriptedCanFlashTransport& t, int jump_outcome = 0)
{
    const auto section = t.Section("in-car connect");
    ScriptDensoPreliminaries(t, 0x00);

    t.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01})); // mismatch logs only

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
    }
}

// erase_memory's setup PDU plus its erase trigger; the trigger's answer is
// consumed by the re-read loop, not by a paired read.
void ScriptEraseMemory(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("erase memory");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block). `encrypted` is the
// whole 0x184000 plan image, encrypted once, and indexed from kImageStart --
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
            t.Exchange(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0x09, 0x11, 0xFE, 0x00, 0x36, 0x1F, 0xFD, 0xD6, 0x57,
                                    0x94, 0x25, 0x90, 0xCA, 0xF5, 0x62, 0x97, 0x50, 0xB5, 0x31, 0xF7, 0xBA, 0xB8, 0x0E,
                                    0xFF, 0x8B, 0xF5, 0xCE, 0x1F, 0x26, 0xA4, 0xDE, 0x83, 0x5E, 0xE0, 0x4A, 0xE9, 0xF8,
                                    0x56, 0x76, 0xE3, 0xEE, 0xD5, 0x0A, 0xBD, 0x69, 0x7E, 0x69, 0x01, 0xD8, 0x99, 0x85,
                                    0x92, 0xBC, 0x49, 0xCF, 0x37, 0x70, 0xC8, 0xF7, 0x06, 0xC9, 0x9F, 0x7C, 0x84, 0x25,
                                    0x4E, 0x4B, 0x11, 0xAB, 0xFF, 0x15, 0xF4, 0x1B, 0x5A, 0x42, 0x56, 0xB1, 0x1F, 0xF0,
                                    0xF7, 0x69, 0xD5, 0x0C, 0x1D, 0xD3, 0x6C, 0xAC, 0xDA, 0x69, 0x9C, 0xE7, 0x98, 0x2B,
                                    0x5D, 0xEE, 0x04, 0x96, 0xE4, 0x4B, 0x69, 0x15, 0x22, 0x2F, 0x47, 0x4E, 0x0A, 0xEC,
                                    0xB0, 0x13, 0x6B, 0x7F, 0xF8, 0x04, 0x1A, 0xBB, 0x4A, 0x11, 0xDB, 0x27, 0xB4, 0x16,
                                    0xC9, 0x6A, 0x3E, 0x1F, 0x61, 0x1C, 0x89, 0xD9, 0x53, 0xCD, 0xCE, 0xC7, 0x03, 0xEC,
                                    0xB2, 0x6B, 0x95, 0x87, 0x3B, 0x5F, 0x2F, 0x7F, 0x49, 0xA3, 0xC8, 0x2B, 0xE5, 0x1B,
                                    0x52, 0x9C, 0xAD, 0x05, 0xB0, 0xBD, 0xB6, 0x32, 0xBC, 0xB4, 0xF5, 0x83, 0xEE, 0x80,
                                    0x6F, 0x1A, 0x76, 0xC6, 0xD9, 0xB5, 0xF9, 0x5A, 0x88, 0x03, 0x95, 0xC2, 0x97, 0xD6,
                                    0xB8, 0x3D, 0x50, 0xA2, 0x0D, 0x30, 0x19, 0x5F, 0x00, 0xD6, 0xDB, 0xA6, 0xB6, 0x9D,
                                    0xB9, 0xC5, 0xAF, 0xC4, 0x3E, 0xF7, 0x17, 0x52, 0x70, 0xAA, 0xA4, 0x49, 0xA7, 0x7B,
                                    0x41, 0x54, 0x03, 0xE9, 0x8D, 0x40, 0x66, 0xA5, 0x5B, 0x9C, 0x15, 0x20, 0xB8, 0xF2,
                                    0xE2, 0x58, 0x7D, 0xD0, 0x31, 0x46, 0x31, 0xD4, 0xB6, 0x25, 0x78, 0x32, 0xB9, 0x44,
                                    0x60, 0x5E, 0x0E, 0x32, 0x36, 0x6C, 0xCE, 0x0B, 0x6B, 0xA2, 0x2A, 0xB2, 0x9D, 0x4D,
                                    0xD5, 0x17, 0x2C, 0x3C, 0xF2, 0x46, 0x30, 0x48, 0xDE, 0x1C, 0x08, 0xE9, 0x29},
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
class SubaruDenso1n83m_1_5mCanExecutorWrite : public testing::TestWithParam<int>
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

TEST(SubaruDenso1n83m_1_5mCanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDenso1n83m_1_5mCanExecutor>(
        ReadPlan(), transport, kImageSize, 0x10000, kBlockLength, 0xA5,
        [](const auto& run, const auto&) { EXPECT_THAT(run.events.logs, testing::Not(IsEmpty())); });
    fastecu::flash::testing::ExpectSeedAndKeyReadTimeouts(transport);
}

// Legacy (20892df) names the 4MB sibling in all three of this family's
// operator-facing identity lines -- an upstream copy-paste. Every other
// identity string in this executor says 1.5M, so a flash log could not be
// attributed to the executor that produced it. Corrected here; this is a
// deliberate divergence from the legacy text, not a transcription slip.
TEST(SubaruDenso1n83m_1_5mCanExecutor, OperatorFacingLogLinesNameThe1_5MFamily)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDenso1n83m_1_5mCanExecutor>(
        ReadPlan(), transport, kImageSize, 0x10000, kBlockLength, 0xA5,
        [](const auto& run, const auto&)
        {
            const auto& logs = run.events.logs;
            EXPECT_THAT(logs, testing::Contains(testing::Pair(
                                  fastecu::LogLevel::kInfo,
                                  "Connecting to Subaru Denso 1N83M 1.5MB CAN bootloader, please wait...")));
            EXPECT_THAT(logs, testing::Contains(testing::Pair(fastecu::LogLevel::kInfo,
                                                              "Reading ROM from Subaru Denso 1N83M 1.5MB using CAN")));
            EXPECT_THAT(logs, testing::Each(testing::Pair(testing::_, testing::Not(testing::HasSubstr("4MB")))));
        });
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptInCarConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDenso1n83m_1_5mCanExecutor>(ReadPlan(), transport, kImageSize,
                                                                                0x10000, kBlockLength, 0x5A);
}

TEST_F(SubaruDenso1n83m_1_5mCanExecutorWrite, WriteErasesThenFlashesBlockOne)
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
        ExecutorRun<SubaruDenso1n83m_1_5mCanExecutor, fastecu::RecordingClock>(*plan, transport);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, testing::Contains("Writing ROM, please wait..."));
    // The write path's own identity line, the third of the three legacy named
    // the 4MB sibling in.
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::kInfo,
                                                             "Writing ROM to Subaru Denso 1N83M 1.5MB using CAN")));
    EXPECT_THAT(events.logs, testing::Each(testing::Pair(testing::_, testing::Not(testing::HasSubstr("4MB")))));
    // The image base is already pinned by the scripted exchanges:
    // scriptReflashChunks builds every 0xB6 frame from encrypted.subspan(addr
    // - kImageStart, 256). This adds only that calculatePayload is 4-byte-word
    // independent, so the page-at-a-time comparison above is a valid way to
    // express it.
    const bytes::Bytes& encrypted = wire_;
    EXPECT_THAT(bytes::Bytes(encrypted.begin() + 0x10000, encrypted.begin() + 0x10000 + 256),
                test_bytes::BytesEq(ToWire(bytes::ByteView(rom_).subspan(0x10000, 256))));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait, the settle after the erase
    // command, and the settle before the checksum-verify write. Asserted as a
    // whole sequence rather than by Contains so that dropping one -- as this
    // port did with the checksum-verify settle -- fails here instead of
    // passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 500ms, 100ms}));
}

// One case for the dialect: all four families run this loop.
// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_memory's 0x34 setup exchange -- a FatalQuery call --
// this pins a transport error raised *inside* the 0xB7 dump-chunk loop, whose
// reads go through FatalRequest at a different call site, inside a `for`
// loop carrying its own cancellation-check and progress-accumulation state.
// FatalRequest's generic error propagation is covered once for every family
// by uds_client_exchange_common_test.cpp; this test is what actually proves
// the loop itself aborts cleanly -- without corrupting rom/progress state --
// on a transport error, rather than assuming FatalRequest's coverage
// implies the loop wrapping it behaves the same way.
TEST(SubaruDenso1n83m_1_5mCanExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDenso1n83m_1_5mCanExecutor>(ReadPlan(), transport,
                                                                                   ErrorKind::kDisconnected);
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, NegativeResponseDuringConnectFails)
{
    // A negative response to the seed request, which returns STATUS_ERROR,
    // must abort rather than be logged and stepped over. The bench connect is
    // the dialect's, so this case stands for all four families.
    ScriptedCanFlashTransport transport;
    ScriptDensoPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x7F, 0x27, 0x35}));

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDenso1n83m_1_5mCanExecutor>(ReadPlan(), transport,
                                                                                   ErrorKind::kBadResponse);
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, NegativeResponseAtDumpSetupFails)
{
    // The mirror image of subaru_denso_1n83m_4m_can_executor_test's
    // ProceedsPast... cases. That family has the `return STATUS_ERROR` at
    // read_memory's 0x34/0x35 setup checks commented out and steps over a bad
    // reply; this one keeps it live, so the very same exchange must abort
    // here. The pair of tests is what stops the difference being normalized
    // away in either direction.
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                       Response({0x7F, 0x34, 0x31}));

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDenso1n83m_1_5mCanExecutor>(ReadPlan(), transport,
                                                                                   ErrorKind::kBadResponse);
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, EmptyBranchSelectorReplyFails)
{
    // An absent 0x22 0x10 0x1D reply is one of only two points in the
    // preliminary phase that return STATUS_ERROR. One case for the dialect.
    ScriptedCanFlashTransport transport;
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    transport.Exchange(Request({0xAA}), Response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));
    transport.Exchange(Request({0x09, 0x02}), Response({0x49, 0x02, 'V', 'I', 'N'}));
    transport.Exchange(Request({0x09, 0x04}), Response({0x49, 0x04, 'C', 'A', 'L'}));
    transport.Exchange(Request({0x09, 0x06}), Response({0x49, 0x06, 0xAA, 0xBB}));
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    transport.Exchange(Request({0x22, 0x10, 0x1D}));
    transport.QueueNoFrame();

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDenso1n83m_1_5mCanExecutor>(ReadPlan(), transport,
                                                                                   ErrorKind::kTimeout);
}

TEST_F(SubaruDenso1n83m_1_5mCanExecutorWrite, EraseRetryExhaustionFails)
{
    // Legacy erase_memory's re-read loop: twenty reads, no re-send, then
    // "Flash area erase failed", and no chunk is written. One case for the
    // dialect; the polling itself is DensoIso15765Erase's.
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.QueueRead(Response({0x71, 0x01, 0x03}));
    }

    const auto plan = WritePlan(rom_);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDenso1n83m_1_5mCanExecutor>(*plan, transport);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    // A second trigger would have hit the end of the script and surfaced as an
    // Internal error rather than BadResponse; the failure is the erase's own.
    EXPECT_THAT(events.logs, testing::Contains(testing::Pair(fastecu::LogLevel::kError, "Flash area erase failed")));
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Denso1n83m_1_5mCanTraits
    : fastecu::flash::testing::CanScriptForwarders<&::ReadPlan, &::HandBuiltPlan, &::ScriptBenchConnect,
                                                   &::ScriptReadSetup, &::ScriptFlashDump, &::ScriptStopCommand,
                                                   &::ScriptUpToFirstFatalRead>
{
    using Executor = SubaruDenso1n83m_1_5mCanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // ConnectBootloader's TolerantProbe exchanges read with this family's
    // short timeout, a deliberate per-family value (subaru_denso_sh72543_can_diesel
    // probes with 2000ms instead).
    static constexpr std::chrono::milliseconds kProbeTimeout{200};
    static constexpr int kProbeCount = 7; // seed and key now read at 2000ms
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDenso1n83m_1_5mCan, CanExecutorConformance, Denso1n83m_1_5mCanTraits);

// The OBK probe and its outcome are the dialect executor's, the same for all
// four families; these two cases stand for all of them. Each family's
// OBK-active log line is pinned by its own ShortPages case.

// An OBK probe that mismatches (0), is absent (1) or is empty (2) is a miss:
// the ECU is initialised and the read proceeds, here until the scripted stop.
TEST(SubaruDenso1n83m_1_5mCanExecutor, ObkMismatchAbsentAndEmptyAreMisses)
{
    for (int probe_case = 0; probe_case < 3; ++probe_case)
    {
        SCOPED_TRACE(probe_case);
        ScriptedCanFlashTransport transport;
        ScriptBenchConnect(transport, probe_case);
        ScriptReadSetup(transport);
        transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), kBlockStart)));
        transport.QueueError(ErrorKind::kDisconnected, "stop after successful connection");
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        auto [clock, events, cancellation, result] = ExecutorRun<SubaruDenso1n83m_1_5mCanExecutor>(*plan, transport);
        EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_THAT(events.logs,
                    testing::Contains(testing::Pair(fastecu::LogLevel::kInfo, "OBK not active, initialising ECU...")));
        EXPECT_EQ(transport.ReadTimeouts().front(), 200ms);
    }
}

// A transport error, a cancellation or a malformed reply to the OBK probe is
// returned as is, before the ECU is initialised.
TEST(SubaruDenso1n83m_1_5mCanExecutor, ObkErrorsAndCancellationPropagateBeforeInitialization)
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
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        auto [clock, events, cancellation, result] = ExecutorRun<SubaruDenso1n83m_1_5mCanExecutor>(*plan, transport);
        EXPECT_THAT(result, fastecu::testing::IsErr(kind));
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_THAT(events.logs, testing::Not(testing::Contains(
                                     testing::Pair(fastecu::LogLevel::kInfo, "OBK not active, initialising ECU..."))));
    }
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort)
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
        ExpectShortPageRead<SubaruDenso1n83m_1_5mCanExecutor>(*plan, transport, exhausted,
                                                              {.block_length = kBlockLength,
                                                               .page_size = kPageSize,
                                                               .lead_pad = 0x10000,
                                                               .obk_active_log = "OBK is active",
                                                               .sleeps = std::vector<std::chrono::milliseconds>{}});
    }
}

// Cancels the moment the read logs its stop request.
class CancelAtReadStopSink final : public RecordingEventSink
{
  public:
    explicit CancelAtReadStopSink(fastecu::ManualCancellationToken& token) : token_(token)
    {
    }
    void Log(fastecu::LogLevel level, std::string_view text) override
    {
        RecordingEventSink::Log(level, text);
        if (text == "Sending stop command")
        {
            token_.Cancel();
        }
    }

  private:
    fastecu::ManualCancellationToken& token_;
};

// Every page is read when the stop is sent, so a cancellation there is
// swallowed and the read returns its image. One case for the dialect.
TEST(SubaruDenso1n83m_1_5mCanExecutor, CancellationAtTheReadStopIsSwallowed)
{
    ScriptedCanFlashTransport transport;
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x5F}));
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    fastecu::ManualCancellationToken cancellation;
    CancelAtReadStopSink events(cancellation);
    RecordingClock clock;
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = SubaruDenso1n83m_1_5mCanExecutor{}.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(result->read_bytes->size(), kImageSize);
    EXPECT_TRUE(cancellation.Cancelled());
    EXPECT_TRUE(transport.ScriptConsumed());
}

// No acknowledgement is required after the bounded bench wait. A matching
// first frame is deliberately discarded in the two-read families.
TEST(SubaruDenso1n83m_1_5mCanExecutor, BenchJumpExhaustionPreservesReadCountAndTiming)
{
    ScriptedCanFlashTransport transport;
    ScriptDensoPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    transport.Exchange(Request({0x10, 0x42}));
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
    SubaruDenso1n83m_1_5mCanExecutor executor;
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                testing::Contains(testing::Pair(fastecu::LogLevel::kError,
                                                "Kernel jump was not acknowledged; continuing, as legacy does")));
    std::vector<std::chrono::milliseconds> sleeps{500ms};
    sleeps.insert(sleeps.end(), 50, 100ms);
    EXPECT_EQ(clock.sleep_calls, sleeps);
    const auto& timeouts = transport.ReadTimeouts();
    ASSERT_GE(timeouts.size(), 64U);
    EXPECT_EQ(timeouts[10], 200ms);
    EXPECT_THAT(std::vector<std::chrono::milliseconds>(timeouts.begin() + 11, timeouts.begin() + 61), Each(200ms));
}

// A whole-image script includes both edge chunks with address-sensitive data;
// N83M's nonzero image base must be subtracted exactly once.
TEST_P(SubaruDenso1n83m_1_5mCanExecutorWrite, CloseRetriesSettleAndChecksumOutcomes)
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
    ExpectCloseOutcome<SubaruDenso1n83m_1_5mCanExecutor>(
        *plan, transport, outcome,
        {.sleeps = std::vector<std::chrono::milliseconds>{500ms, 500ms, 100ms},
         .checksum_timeout = 500ms,
         .reread_timeout = 500ms});
}

INSTANTIATE_TEST_SUITE_P(ChecksumOutcomes, SubaruDenso1n83m_1_5mCanExecutorWrite, testing::Range(0, 5),
                         testing::PrintToStringParamName());

TEST(SubaruDenso1n83m_1_5mCanExecutor, IdentityExchangeFailuresIncludingCancellationAreNonFatal)
{
    fastecu::flash::testing::ExpectIdentityFailuresAreNonFatal<SubaruDenso1n83m_1_5mCanExecutor>(
        &ReadPlan, {0xAA}, {200ms, 2000ms, 2000ms, 2000ms, 2000ms, 200ms});
}

TEST(SubaruDenso1n83m_1_5mCanExecutor, InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy)
{
    for (const int outcome : {1, 2})
    {
        SCOPED_TRACE(::testing::PrintToString(outcome));
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
        SubaruDenso1n83m_1_5mCanExecutor executor;
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
