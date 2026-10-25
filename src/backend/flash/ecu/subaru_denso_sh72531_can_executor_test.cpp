#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for SubaruDensoSh72531CanExecutor, the portable
// replacement for FlashEcuSubaruDensoSH72531CanOperation's
// connect_bootloader(), read_memory(), write_memory(), reflash_block() and
// erase_memory(). Every scripted exchange cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh72531_can_operation.cpp
// it was transcribed from, as of 20892df -- this branch's base, and the last
// commit before the file was deleted here. Master later reflowed one line in
// three of the four (S1117, #240), so resolve citations against 20892df.
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
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
using fastecu::LogLevel;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::flash::BuildSubaruDensoSh72531CanPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDensoSh72531CanExecutor;
using fastecu::flash::SubaruDensoSh72531CanPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
using testing::Contains;
using testing::Each;
using ::testing::Eq;
using testing::IsEmpty;
using testing::Pair;

constexpr std::string_view kProtocol = "sub_ecu_denso_sh72531_can";
constexpr std::string_view kMcu = "SH72531";

// kFlashBlocksSH72531: [0] = {0x00000000, 0x8000}, [1] = {0x00008000, 0x137F00},
// [2] = {0x00137F00, 0x100}.
constexpr std::uint32_t kImageStart = 0x00000000;
constexpr std::uint32_t kBlockStart = 0x00008000;
constexpr std::uint32_t kBlockLength = 0x137F00;
constexpr std::size_t kImageSize = 0x140000;
constexpr std::uint32_t kPageSize = 0x100;

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
{
    return BuildSubaruDensoSh72531CanPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruDensoSh72531CanPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
{
    return fastecu::flash::testing::HandBuiltCanPlan(
        operation, fastecu::flash::FlashFamily::kSubaruDensoSh72531Can, kProtocol, kMcu,
        fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}, kImageSize,
        SubaruDensoSh72531CanPlan{0x7e0, 0x7e8, 500000, false, 0x8000, 0x100});
}

// The bench arm. Its kernel jump reads twice and examines only the second
// reply, so the deliberately non-matching first frame below pins that the port
// discards it.
void ScriptBenchConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("bench connect");
    ScriptDensoPreliminaries(t, 0xFF);
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    t.Exchange(Request({0x10, 0x42}), Response({0x7F, 0x10, 0x78})); // discarded by the re-read
    t.QueueRead(Response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory).
void ScriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x35, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}),
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
    t.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}));
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
    t.Exchange(RequestTo(0x7E1, {0x10, 0x03}), ResponseFrom(0x7E9, {0x50, 0x03}));
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
    }
}

// erase_memory's setup PDU plus its erase trigger; the trigger's answer is
// consumed by the re-read loop, not by a paired read.
void ScriptEraseMemory(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("erase memory");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block). `encrypted` is the
// whole 0x140000 plan image, encrypted once, and indexed from kImageStart = 0
// -- so chunk 0 carries encrypted[0x8000..0x8100).
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
            t.Exchange(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0x00, 0x00, 0x80, 0x00, 0xA9, 0x8E, 0x7B, 0x6D, 0xF9,
                                    0x1E, 0xCA, 0xAC, 0xF3, 0xDE, 0x61, 0x5E, 0xA5, 0xEB, 0x06, 0xFF, 0x59, 0x0B, 0xBB,
                                    0x01, 0xFF, 0x87, 0x61, 0xC3, 0x20, 0x76, 0xD1, 0x38, 0x63, 0xD7, 0x6E, 0x2E, 0x17,
                                    0xC5, 0x08, 0x7D, 0x22, 0x45, 0xCD, 0x38, 0xFB, 0x0B, 0x6D, 0x15, 0x56, 0xDC, 0x3E,
                                    0x29, 0x5B, 0x10, 0xA2, 0x31, 0xB7, 0x50, 0xE0, 0xE2, 0x22, 0x5C, 0x00, 0xD3, 0x41,
                                    0x6D, 0x80, 0x71, 0x38, 0x53, 0xD1, 0xBD, 0xAF, 0x67, 0xEC, 0xB0, 0xC3, 0x16, 0xD2,
                                    0x3E, 0x48, 0x48, 0x93, 0xDA, 0x50, 0xE9, 0x51, 0x1D, 0xCB, 0x75, 0x20, 0x00, 0x24,
                                    0xF0, 0x77, 0x10, 0x15, 0x7D, 0x2B, 0x99, 0xE7, 0x5A, 0x2C, 0x7A, 0x1E, 0xC8, 0x83,
                                    0x8C, 0xCA, 0xB7, 0x2F, 0x14, 0xA5, 0xC7, 0x4F, 0x5C, 0x74, 0x9A, 0xD3, 0xFC, 0x0C,
                                    0x98, 0x26, 0x6D, 0x2A, 0x4A, 0x9E, 0x1C, 0xC5, 0xFE, 0x4B, 0x39, 0xE0, 0xD3, 0x85,
                                    0x2B, 0x95, 0xDE, 0x83, 0x5A, 0xAE, 0x20, 0x2D, 0xB2, 0x72, 0x5A, 0x3F, 0x6D, 0xB0,
                                    0x2D, 0xCE, 0x6A, 0x13, 0xCD, 0x0F, 0x86, 0x20, 0x47, 0x6A, 0x1B, 0x80, 0xAA, 0x24,
                                    0x85, 0x15, 0xD9, 0xA5, 0x80, 0x49, 0xE3, 0x7D, 0x03, 0x05, 0x59, 0xDA, 0x6D, 0x59,
                                    0x52, 0x6E, 0xFF, 0xD5, 0x19, 0x1C, 0x20, 0x2C, 0xCC, 0x48, 0x1F, 0xBA, 0x62, 0xAF,
                                    0xA6, 0xE0, 0x01, 0xEB, 0x90, 0x1E, 0x88, 0xF9, 0x1E, 0xAE, 0xBC, 0x5C, 0x8A, 0x90,
                                    0x71, 0xE8, 0x4C, 0x1B, 0xDD, 0x70, 0x62, 0xBA, 0x4F, 0x12, 0xA1, 0x12, 0xEC, 0xD5,
                                    0xED, 0xD5, 0xEF, 0x20, 0xB1, 0x1D, 0xC5, 0x0C, 0xCC, 0xF9, 0x2E, 0x10, 0xBD, 0xCB,
                                    0xBF, 0xB5, 0x49, 0x52, 0x26, 0x4B, 0xEB, 0x27, 0xDA, 0xD0, 0x9B, 0xFF, 0x17, 0x7F,
                                    0xE8, 0x88, 0xF9, 0x81, 0x41, 0xA7, 0x94, 0x8B, 0xE2, 0x1A, 0xEB, 0x15, 0x40},
                       Response({0xF6}));
        }
        else if (offset == kBlockLength - 256)
        {
            t.Exchange(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0x00, 0x13, 0xFE, 0x00, 0x7A, 0xBE, 0xB7, 0xDA, 0x81,
                                    0x68, 0xD4, 0xAE, 0x6E, 0x19, 0xF2, 0x3F, 0x98, 0xAC, 0xDF, 0xA3, 0x73, 0x41, 0xEA,
                                    0xED, 0x7B, 0x23, 0x9B, 0x36, 0x43, 0xC4, 0x57, 0x5F, 0x72, 0xB1, 0xFB, 0xD9, 0xF4,
                                    0xE2, 0x5D, 0x9E, 0x8A, 0xEE, 0xFB, 0xF7, 0x51, 0xA6, 0x4A, 0xE4, 0xFD, 0x9C, 0xC2,
                                    0x89, 0x8C, 0xEC, 0x43, 0x50, 0xC8, 0xC0, 0x76, 0xE1, 0x49, 0x16, 0xC0, 0xBB, 0x74,
                                    0xF8, 0xC2, 0xEC, 0xBB, 0x37, 0x94, 0x70, 0x9F, 0xB9, 0xD3, 0xA5, 0x50, 0x87, 0xF4,
                                    0x76, 0x66, 0x28, 0xD3, 0x6A, 0x74, 0x7D, 0x34, 0xDF, 0x3C, 0x03, 0x51, 0x00, 0x52,
                                    0x3A, 0x4E, 0x16, 0xCB, 0x5D, 0xCB, 0x94, 0xC4, 0x93, 0x94, 0x02, 0x00, 0xBD, 0x32,
                                    0x08, 0xB3, 0x7F, 0xA9, 0x3A, 0x8C, 0xB4, 0x33, 0x1B, 0x45, 0x9A, 0x98, 0x25, 0xDB,
                                    0x69, 0xFF, 0xAD, 0x2B, 0x34, 0xE7, 0xCF, 0x09, 0xB9, 0x8B, 0x60, 0x94, 0xA0, 0x94,
                                    0xA7, 0xFB, 0xE5, 0xCD, 0xF2, 0xE4, 0x9C, 0xEF, 0x47, 0x9D, 0x46, 0x3A, 0x3A, 0x4D,
                                    0xD5, 0x43, 0x56, 0x65, 0xEE, 0x37, 0x94, 0x25, 0x33, 0x03, 0xD4, 0xF2, 0xFA, 0xE8,
                                    0x84, 0x65, 0x7A, 0x8C, 0x47, 0x20, 0x9D, 0x89, 0x1E, 0x67, 0x23, 0x4C, 0xFD, 0xEA,
                                    0x8F, 0x02, 0xB3, 0x06, 0x7B, 0x43, 0x9A, 0xA4, 0x69, 0x45, 0x34, 0xF1, 0x2D, 0x74,
                                    0x88, 0x00, 0xF6, 0x8B, 0x7B, 0x36, 0xA3, 0x19, 0x21, 0xFE, 0xF7, 0x73, 0x6C, 0xD3,
                                    0xE6, 0x24, 0xFE, 0x21, 0xB3, 0x83, 0x27, 0x8F, 0x78, 0x8B, 0x3D, 0xF6, 0xC6, 0xCA,
                                    0x43, 0x0B, 0xC2, 0x5A, 0x23, 0xC3, 0x72, 0xC3, 0xF9, 0x01, 0x26, 0x5F, 0xA8, 0xE5,
                                    0x45, 0xC6, 0xEE, 0x65, 0x55, 0x00, 0xD6, 0x43, 0xE6, 0x15, 0x9E, 0x31, 0x84, 0xF9,
                                    0x73, 0xD8, 0x79, 0xC6, 0x0F, 0xF9, 0xD3, 0xCA, 0x36, 0x81, 0x38, 0x9F, 0xE2},
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
class SubaruDensoSh72531CanExecutorWrite : public testing::TestWithParam<int>
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

TEST(SubaruDensoSh72531CanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDensoSh72531CanExecutor>(
        ReadPlan(), transport, kImageSize, 0x8000, kBlockLength, 0xA5,
        [](const auto& run, const auto&)
        {
            EXPECT_THAT(
                run.events.logs,
                Contains(Pair(LogLevel::kInfo, "Connecting to Subaru Denso SH72531 CAN bootloader, please wait...")));
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "Reading ROM from Subaru Denso SH72531 using CAN")));
            EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "OBK not active, initialising ECU...")));
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "Bench programming: accessing, please wait...")));
        });
    fastecu::flash::testing::ExpectSeedAndKeyReadTimeouts(transport);
}

TEST(SubaruDensoSh72531CanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptInCarConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDensoSh72531CanExecutor>(
        ReadPlan(), transport, kImageSize, 0x8000, kBlockLength, 0x5A,
        [](const auto& run, const auto&)
        {
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "In car programming: accessing, please wait...")));
        });
}

TEST_F(SubaruDensoSh72531CanExecutorWrite, WriteErasesThenFlashesBlockOne)
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
        ExecutorRun<SubaruDensoSh72531CanExecutor, fastecu::RecordingClock>(*plan, transport);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Writing ROM to Subaru Denso SH72531 using CAN")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Flash erased! Starting flash write, do not power off!")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Closing out Flashing of this block")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Checksum verified")));
    // Every 0xB6 chunk was matched byte-for-byte by the scripted exchanges
    // above; assert the indexing convention explicitly too, so a wrong image
    // base fails here with a readable message rather than as an "unexpected
    // write".
    const bytes::Bytes& encrypted = wire_;
    EXPECT_THAT(bytes::Bytes(encrypted.begin() + 0x8000, encrypted.begin() + 0x8000 + 256),
                test_bytes::BytesEq(ToWire(bytes::ByteView(rom_).subspan(0x8000, 256))));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait, the bench kernel jump's
    // inter-read settle, the settle after the erase command, and the settle
    // before the checksum-verify write. Asserted as a whole sequence rather
    // than by Contains so that dropping one -- as this port did with both the
    // kernel-jump and the checksum-verify settle -- fails here instead of
    // passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 50ms, 500ms, 100ms}));
}

TEST(SubaruDensoSh72531CanExecutor, BenchKernelJumpDiscardsFirstReply)
{
    // Unique to this family among the bootloader dialect's 1N83M pair: after the 0x10 0x42
    // write the bench arm reads twice and keeps only the second reply (with a
    // delay(50) between them), where the 1N83M 1.5M sibling reads once. Its
    // 1N83M 4M sibling does the same two reads. Both scripted frames are
    // consumed either way, so the discard is pinned by the clock instead: the
    // acknowledgement is found on the loop's first look, so the whole connect
    // sleeps exactly twice -- connect_bench's own 500 ms wait and the jump's
    // 50 ms inter-read settle. Reading once would drop the 50, and pushing the
    // ack into a second loop iteration would add the loop's 100 ms retry
    // sleep.
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}));
    transport.QueueError(ErrorKind::kTimeout, "stop after connect");

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] =
        ExecutorRun<SubaruDensoSh72531CanExecutor, fastecu::RecordingClock>(*plan, transport);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Kernel jump acknowledged")));
    // connect_bench's 500 ms wait then the jump's own 50 ms between its two
    // reads.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{500ms, 50ms}));
    EXPECT_EQ(clock.Elapsed(), 550ms);
}

TEST(SubaruDensoSh72531CanExecutor, NegativeResponseAtDumpSetupFails)
{
    // The mirror image of subaru_denso_1n83m_4m_can_executor_test's
    // ProceedsPast... cases. That family has the `return STATUS_ERROR` at
    // read_memory's 0x34/0x35 setup checks commented out and steps over a bad
    // reply; this one keeps it live, so the very same exchange must abort
    // here. The pair of tests is what stops the difference being normalized
    // away in either direction.
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}),
                       Response({0x7F, 0x34, 0x31}));

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDensoSh72531CanExecutor>(ReadPlan(), transport,
                                                                                ErrorKind::kBadResponse);
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Sh72531CanTraits
    : fastecu::flash::testing::CanScriptForwarders<&::ReadPlan, &::HandBuiltPlan, &::ScriptBenchConnect,
                                                   &::ScriptReadSetup, &::ScriptFlashDump, &::ScriptStopCommand,
                                                   &::ScriptUpToFirstFatalRead>
{
    using Executor = SubaruDensoSh72531CanExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // serial_read_short_timeout (header lines 45-48): the timeout the
    // TolerantProbe exchanges in ConnectBootloader read with, and the one
    // every other short-timeout read in this family's connect/read path
    // shares.
    static constexpr std::chrono::milliseconds kProbeTimeout{200};
    // The dump sweep's own reads use receive_timeout, not this one.
    static constexpr int kProbeCount = 6; // seed and key now read at 2000ms
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDensoSh72531Can, CanExecutorConformance, Sh72531CanTraits);

TEST(SubaruDensoSh72531CanExecutor, ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort)
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
        ExpectShortPageRead<SubaruDensoSh72531CanExecutor>(
            *plan, transport, exhausted,
            {.block_length = kBlockLength,
             .page_size = kPageSize,
             .lead_pad = 0x8000,
             .obk_active_log = "OBK is active!",
             .sleeps = std::vector<std::chrono::milliseconds>(kBlockLength / kPageSize, 1ms)});
    }
}

// No acknowledgement is required after the bounded bench wait. A matching
// first frame is deliberately discarded in the two-read families.
TEST(SubaruDensoSh72531CanExecutor, BenchJumpExhaustionPreservesReadCountAndTiming)
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
    SubaruDensoSh72531CanExecutor executor;
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

// A whole-image script includes both edge chunks with address-sensitive data;
// N83M's nonzero image base must be subtracted exactly once.
TEST_P(SubaruDensoSh72531CanExecutorWrite, CloseRetriesSettleAndChecksumOutcomes)
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
    ExpectCloseOutcome<SubaruDensoSh72531CanExecutor>(
        *plan, transport, outcome,
        {.sleeps = std::vector<std::chrono::milliseconds>{500ms, 50ms, 500ms, 100ms},
         .checksum_timeout = 2000ms,
         .reread_timeout = 2000ms});
}

INSTANTIATE_TEST_SUITE_P(ChecksumOutcomes, SubaruDensoSh72531CanExecutorWrite, testing::Range(0, 5),
                         testing::PrintToStringParamName());

TEST(SubaruDensoSh72531CanExecutor, IdentityExchangeFailuresIncludingCancellationAreNonFatal)
{
    fastecu::flash::testing::ExpectIdentityFailuresAreNonFatal<SubaruDensoSh72531CanExecutor>(
        &ReadPlan, {0xAA}, {200ms, 2000ms, 2000ms, 2000ms, 2000ms, 200ms});
}

TEST(SubaruDensoSh72531CanExecutor, InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy)
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
        SubaruDensoSh72531CanExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(outcome != 2 ? ErrorKind::kDisconnected : ErrorKind::kCancelled));
        EXPECT_TRUE(transport.ScriptConsumed());
        const auto& timeouts = transport.ReadTimeouts();
        const std::size_t count = outcome == 1 ? 11 : outcome == 3 ? 2 : 1;
        const std::size_t trailing_read_count = outcome != 2 ? 3 : 0;
        ASSERT_GE(timeouts.size(), count + trailing_read_count);
        using TimeoutDifference = std::vector<std::chrono::milliseconds>::difference_type;
        const auto end = timeouts.end() - static_cast<TimeoutDifference>(trailing_read_count);
        EXPECT_THAT(std::vector<std::chrono::milliseconds>(end - static_cast<TimeoutDifference>(count), end),
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
} // namespace
