#include "src/algorithms/protocol/testing/byte_matchers.h"
// Equivalence tests for SubaruDensoSh72543CanDieselExecutor, the portable
// replacement for flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp.
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/ecu/testing/can_executor_run.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::flash::BuildSubaruDensoSh72543CanDieselPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDensoSh72543CanDieselExecutor;
using fastecu::flash::SubaruDensoSh72543CanDieselPlan;
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

constexpr std::string_view kProtocol = "sub_ecu_denso_sh72543_can_diesel";
constexpr std::string_view kMcu = "SH72543d";

// kFlashBlocksSH72543d is the bootloader dialect's only single-block table: [0] = {0x00008000,
// 0x1F7F00}, with the 0x0-0x8000 entry commented out. The image is still based
// at address 0 -- read_memory prepends 0x8000 of 0xFF, and write_memory
// offsets its buffer pointer by fblocks[0].start before reflash_block indexes
// it block-relative.
constexpr std::uint32_t kImageStart = 0x00000000;
constexpr std::uint32_t kBlockStart = 0x00008000;
constexpr std::uint32_t kBlockLength = 0x1F7F00;
constexpr std::size_t kImageSize = 0x200000;
constexpr std::uint32_t kPageSize = 0x100;

// Writes scripted by scriptBenchConnect and scriptEraseMemory. Named so
// WriteTakesBytesFromTheAbsoluteAddress can assert exactly how far the
// executor got.
constexpr std::size_t kBenchConnectWrites = 11;
constexpr std::size_t kEraseWrites = 2;

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan()
{
    return BuildSubaruDensoSh72543CanDieselPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom)
{
    return BuildSubaruDensoSh72543CanDieselPlan(FlashOperation::kWrite, kProtocol, kMcu, std::move(rom));
}

fastecu::Result<fastecu::flash::FlashPlan> HandBuiltPlan(FlashOperation operation)
{
    return fastecu::flash::testing::HandBuiltCanPlan(
        operation, fastecu::flash::FlashFamily::kSubaruDensoSh72543CanDiesel, kProtocol, kMcu,
        fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}, kImageSize,
        SubaruDensoSh72543CanDieselPlan{0x7e0, 0x7e8, 500000, false, 0x8000, 0x100});
}

// This family asks for its ECU id with ReadDataByIdentifier 0xF182.
void ScriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branch_byte)
{
    ScriptDensoPreliminaries(t, branch_byte, 0, true);
}

// The bench arm. Its kernel jump reads once before the loop, where the SH72531
// sibling reads twice.
void ScriptBenchConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("bench connect");
    ScriptPreliminaries(t, 0xFF);
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    t.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory). This family computes the
// address and length bytes from its region rather than emitting them as
// literals; the resulting wire bytes are what is scripted here.
void ScriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("read setup");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x35, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
               Response({0x75, 0x20, 0x01, 0x01}));
}

// Connect plus read_memory's 0x34 dump-setup request, registered as an
// expected write only -- its reply is left for the caller to queue, so the
// same script serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there. Unlike its 1N83M 4M sibling, this family keeps read_memory's
// 0x34/0x35 setup checks live (NegativeResponseAtDumpSetupFails), so this
// point is safe to use here.
void ScriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    ScriptBenchConnect(t);
    const auto section = t.Section("read setup (first request only)");
    t.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
}

// The in-car arm. The ten fire-and-forget replies are deliberately given
// arbitration ids other than 0x7E8 wherever the addressed module would answer
// on its own id: legacy reads whichever frame arrives next without checking
// the id, and this pins that the port does not add a check legacy lacks.
void ScriptInCarConnect(ScriptedCanFlashTransport& t, int jump_outcome = 0)
{
    const auto section = t.Section("in-car connect");
    ScriptPreliminaries(t, 0x00);

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
    t.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
               Response({0x74, 0x20, 0x01, 0x05}));
    t.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 0 (legacy reflash_block). `encrypted` is the
// whole 0x200000 plan image, encrypted once, and indexed from kImageStart = 0
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
            t.Exchange(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0x00, 0x1F, 0xFE, 0x00, 0x98, 0xAC, 0xDF, 0xA3, 0x6E,
                                    0x19, 0xF2, 0x3F, 0x81, 0x68, 0xD4, 0xAE, 0x7A, 0xBE, 0xB7, 0xDA, 0x72, 0xB1, 0xFB,
                                    0xD9, 0x43, 0xC4, 0x57, 0x5F, 0x7B, 0x23, 0x9B, 0x36, 0x73, 0x41, 0xEA, 0xED, 0xFD,
                                    0x9C, 0xC2, 0x89, 0x51, 0xA6, 0x4A, 0xE4, 0x8A, 0xEE, 0xFB, 0xF7, 0xF4, 0xE2, 0x5D,
                                    0x9E, 0x74, 0xF8, 0xC2, 0xEC, 0x49, 0x16, 0xC0, 0xBB, 0xC8, 0xC0, 0x76, 0xE1, 0x8C,
                                    0xEC, 0x43, 0x50, 0x66, 0x28, 0xD3, 0x6A, 0x50, 0x87, 0xF4, 0x76, 0x9F, 0xB9, 0xD3,
                                    0xA5, 0xBB, 0x37, 0x94, 0x70, 0xCB, 0x5D, 0xCB, 0x94, 0x52, 0x3A, 0x4E, 0x16, 0x3C,
                                    0x03, 0x51, 0x00, 0x74, 0x7D, 0x34, 0xDF, 0x8C, 0xB4, 0x33, 0x1B, 0xB3, 0x7F, 0xA9,
                                    0x3A, 0x00, 0xBD, 0x32, 0x08, 0xC4, 0x93, 0x94, 0x02, 0x09, 0xB9, 0x8B, 0x60, 0x2B,
                                    0x34, 0xE7, 0xCF, 0xDB, 0x69, 0xFF, 0xAD, 0x45, 0x9A, 0x98, 0x25, 0x9D, 0x46, 0x3A,
                                    0x3A, 0xE4, 0x9C, 0xEF, 0x47, 0xFB, 0xE5, 0xCD, 0xF2, 0x94, 0xA0, 0x94, 0xA7, 0xF2,
                                    0xFA, 0xE8, 0x84, 0x25, 0x33, 0x03, 0xD4, 0x65, 0xEE, 0x37, 0x94, 0x4D, 0xD5, 0x43,
                                    0x56, 0xEA, 0x8F, 0x02, 0xB3, 0x67, 0x23, 0x4C, 0xFD, 0x20, 0x9D, 0x89, 0x1E, 0x65,
                                    0x7A, 0x8C, 0x47, 0x00, 0xF6, 0x8B, 0x7B, 0xF1, 0x2D, 0x74, 0x88, 0xA4, 0x69, 0x45,
                                    0x34, 0x06, 0x7B, 0x43, 0x9A, 0x21, 0xB3, 0x83, 0x27, 0xD3, 0xE6, 0x24, 0xFE, 0xFE,
                                    0xF7, 0x73, 0x6C, 0x36, 0xA3, 0x19, 0x21, 0xC3, 0x72, 0xC3, 0xF9, 0x0B, 0xC2, 0x5A,
                                    0x23, 0xF6, 0xC6, 0xCA, 0x43, 0x8F, 0x78, 0x8B, 0x3D, 0x43, 0xE6, 0x15, 0x9E, 0x65,
                                    0x55, 0x00, 0xD6, 0xE5, 0x45, 0xC6, 0xEE, 0x01, 0x26, 0x5F, 0xA8, 0x81, 0x38, 0x9F,
                                    0xE2, 0xF9, 0xD3, 0xCA, 0x36, 0xD8, 0x79, 0xC6, 0x0F, 0x31, 0x84, 0xF9, 0x73},
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
class SubaruDensoSh72543CanDieselExecutorWrite : public testing::TestWithParam<int>
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

TEST(SubaruDensoSh72543CanDieselExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDensoSh72543CanDieselExecutor>(
        ReadPlan(), transport, kImageSize, 0x8000, kBlockLength, 0xA5,
        [](const auto& run, const auto&)
        {
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo,
                                      "Connecting to Subaru Denso SH72543 Diesel CAN bootloader, please wait...")));
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "Reading ROM from Subaru Denso SH72543 Diesel using CAN")));
            EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "OBK not active, initialising ECU...")));
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "Bench programming: accessing, please wait...")));
            // Legacy's own wording for this line, lowercase-hex and unpadded because
            // it is built with QString::number(x, 16).
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "Setting flash start: 0x8000 and length: 0x1f7f00")));
        });
    fastecu::flash::testing::ExpectSeedAndKeyReadTimeouts(transport);
}

TEST(SubaruDensoSh72543CanDieselExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    ScriptInCarConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    ScriptStopCommand(transport);

    fastecu::flash::testing::ExpectPaddedRead<SubaruDensoSh72543CanDieselExecutor>(
        ReadPlan(), transport, kImageSize, 0x8000, kBlockLength, 0x5A,
        [](const auto& run, const auto&)
        {
            EXPECT_THAT(run.events.logs,
                        Contains(Pair(LogLevel::kInfo, "In car programming: accessing, please wait...")));
        });
}

TEST_F(SubaruDensoSh72543CanDieselExecutorWrite, WriteErasesThenFlashesBlockZero)
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
        ExecutorRun<SubaruDensoSh72543CanDieselExecutor, fastecu::RecordingClock>(*plan, transport);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Writing ROM to Subaru Denso SH72543 Diesel using CAN")));
    // The block index legacy reports, which is 0 for this family's single-
    // block table.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Blocks to flash: 0,  (total: 1)")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Block 0 reflash complete.")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Flash erased! Starting flash write, do not power off!")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Closing out Flashing of this block")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Checksum verified")));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait (50 ms here, where the three
    // sibling families wait 500), the settle after the erase command, and the
    // settle before the checksum-verify write. Asserted as a whole sequence
    // rather than by Contains so that dropping one fails here instead of
    // passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{50ms, 500ms, 100ms}));
}

TEST(SubaruDensoSh72543CanDieselExecutor, WriteTakesBytesFromTheAbsoluteAddress)
{
    // A 0x200000 image whose byte at 0x8000 is 0xC3 must put 0xC3 in the first
    // written chunk -- not the byte at offset 0. Legacy composes the same
    // indexing out of two halves: write_memory offsets the encrypted buffer by
    // fblocks[0].start = 0x8000 and reflash_block then indexes it block-
    // relative, which together address encrypted[block_addr + i]. Only the
    // first 0xB6 chunk is scripted, so the executor stops on the second with
    // an "unexpected scripted CAN write"; the write count is what pins the
    // base -- an image based at 0x8000 would have failed one write earlier, on
    // the first chunk.
    bytes::Bytes rom(kImageSize, 0x00);
    rom[0x8000] = 0xC3;

    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptEraseMemory(transport);
    transport.QueueRead(Response({0x71, 0x01, 0x02}));
    const bytes::Bytes encrypted = ToWire(rom);
    transport.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB6), kBlockStart,
                                                bytes::ByteView(encrypted).subspan(kBlockStart - kImageStart, 256))),
                       Response({0xF6}));

    const auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDensoSh72543CanDieselExecutor>(*plan, transport);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_EQ(transport.WritesConsumed(), kBenchConnectWrites + kEraseWrites + 1);
    EXPECT_TRUE(transport.ScriptConsumed());
    // Independent of the executor: the planted byte really does distinguish
    // the two candidate bases, so the assertion above cannot pass vacuously.
    EXPECT_NE(ToWire(bytes::ByteView(rom).subspan(0x8000, 256)), ToWire(bytes::ByteView(rom).first(256)));
}

TEST(SubaruDensoSh72543CanDieselExecutor, BenchSessionMismatchIsToleratedAndContinues)
{
    // Unique to this family: the bench arm's `10 43` check carries a
    // commented-out `return STATUS_ERROR`, so a wrong `50 43` answer is logged
    // and stepped over where all three sibling families abort. The connect
    // must therefore reach the seed request, which is the next write scripted
    // below.
    ScriptedCanFlashTransport transport;
    ScriptPreliminaries(transport, 0xFF);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x01})); // wrong subfunction, tolerated
    transport.Exchange(Request({0x27, 0x61}));
    transport.QueueError(ErrorKind::kTimeout, "stop after the tolerated mismatch");

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDensoSh72543CanDieselExecutor>(*plan, transport);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    // The mismatch was logged, not swallowed silently...
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Wrong response from ECU: 50 01 ")));
    // ...and the sequence carried on past it into the seed request.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Starting seed request")));
}

// Kept alongside the generic conformance ReadTimeoutPropagates (which uses
// the same scriptUpToFirstFatalRead point) because this pins facts that test
// does not: the bench arm's own connect-timing shape. Its kernel jump reads
// only once (unlike the SH72531 sibling's two), so the connect's only sleep
// is the bench arm's 50 ms wait -- the kernel jump's 100 ms retry sleep is
// never reached because the single read is acknowledged immediately.
TEST(SubaruDensoSh72543CanDieselExecutor, ReadTimeoutAfterConnectOnlySleepsTheBenchWait)
{
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
    transport.QueueError(ErrorKind::kTimeout, "no reply");

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDensoSh72543CanDieselExecutor>(*plan, transport);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Kernel jump acknowledged")));
    EXPECT_EQ(clock.Elapsed(), 50ms);
}

TEST(SubaruDensoSh72543CanDieselExecutor, NegativeResponseAtDumpSetupFails)
{
    // The mirror image of subaru_denso_1n83m_4m_can_executor_test's
    // ProceedsPast... cases, and the same test the 1N83M 1.5M and SH72531
    // ports carry. This family keeps read_memory's 0x34/0x35 setup checks
    // live, so a negative answer must abort here where the 4M steps over it.
    // The pair of tests is what stops the difference being normalized away in
    // either direction. The scripted 0x34 bytes are computed from the region
    // rather than literal in this family -- see scriptReadSetup -- so they are
    // spelled out here the same way.
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
                       Response({0x7F, 0x34, 0x31}));

    fastecu::flash::testing::ExpectReadFailsWith<SubaruDensoSh72543CanDieselExecutor>(ReadPlan(), transport,
                                                                                      ErrorKind::kBadResponse);
}

TEST(SubaruDensoSh72543CanDieselExecutor, EcuIdLogKeepsOnlyLegacysFiveBytes)
{
    // Legacy strips the 4-byte envelope and `62 F1 82` with remove(0, 7) and
    // leaves the remainder intact -- its remove(5, len-5) is commented out --
    // but the string it logs is built by `for (int i = 0; i < 5; i++)`, so a
    // longer reply is still logged as five bytes. Script eight and expect
    // five.
    ScriptedCanFlashTransport transport;
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    transport.Exchange(Request({0x22, 0xF1, 0x82}),
                       Response({0x62, 0xF1, 0x82, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}));
    transport.Exchange(Request({0x09, 0x02}));
    transport.QueueNoFrame();

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto [clock, events, cancellation, result] = ExecutorRun<SubaruDensoSh72543CanDieselExecutor>(*plan, transport);

    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInternal,
                                                    ::testing::HasSubstr("ran past the end of the script")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "ECU ID: 11 22 33 44 55 ")));
}

// Unlike this family's kProbeTimeout (2000ms, see the conformance suite's
// ConnectProbesReadWithThisFamilysProbeTimeout below), a full bench-connect +
// read-setup + full dump + stop-command run also reads with the plain short
// timeout (serial_read_short_timeout) exactly three times: the OBK probe and
// read_memory's own 0x34/0x35 setup pair. Kept as its own fact -- distinct
// from, not a restatement of, the family's probe timeout.
TEST(SubaruDensoSh72543CanDieselExecutor, ObkProbeAndReadSetupReadWithTheShortTimeout)
{
    using namespace std::chrono_literals;
    ScriptedCanFlashTransport transport;
    ScriptBenchConnect(transport);
    ScriptReadSetup(transport);
    ScriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    ScriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_EQ(std::ranges::count(transport.ReadTimeouts(), 200ms), 3);
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Sh72543CanDieselTraits
    : fastecu::flash::testing::CanScriptForwarders<&::ReadPlan, &::HandBuiltPlan, &::ScriptBenchConnect,
                                                   &::ScriptReadSetup, &::ScriptFlashDump, &::ScriptStopCommand,
                                                   &::ScriptUpToFirstFatalRead>
{
    using Executor = SubaruDensoSh72543CanDieselExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // ConnectBootloader's TolerantProbe exchanges read with this family's
    // long timeout (serial_read_timeout), unlike its three siblings, which
    // probe with the short one.
    static constexpr std::chrono::milliseconds kProbeTimeout{2000};
    // Unlike its three siblings, this family's own read_memory dump-chunk
    // loop (the 0xB7 sweep) ALSO reads with kProbeTimeout (both are
    // kLongPolicy/serial_read_timeout), so this count is dominated by the
    // roughly 8063 dump-chunk reads (kBlockLength / kPageSize) rather than
    // by the handful of connect-time probes --
    // ObkProbeAndReadSetupReadWithTheShortTimeout above is what isolates the
    // small, human-checkable connect-time count instead.
    static constexpr int kProbeCount = 8073;
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDensoSh72543CanDiesel, CanExecutorConformance, Sh72543CanDieselTraits);

TEST(SubaruDensoSh72543CanDieselExecutor, ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort)
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
        ExpectShortPageRead<SubaruDensoSh72543CanDieselExecutor>(
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
TEST(SubaruDensoSh72543CanDieselExecutor, BenchJumpExhaustionPreservesReadCountAndTiming)
{
    ScriptedCanFlashTransport transport;
    ScriptPreliminaries(transport, 0xFF);
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
    SubaruDensoSh72543CanDieselExecutor executor;
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                testing::Contains(testing::Pair(fastecu::LogLevel::kError,
                                                "Kernel jump was not acknowledged; continuing, as legacy does")));
    std::vector<std::chrono::milliseconds> sleeps{50ms};
    sleeps.insert(sleeps.end(), 50, 100ms);
    EXPECT_EQ(clock.sleep_calls, sleeps);
    const auto& timeouts = transport.ReadTimeouts();
    ASSERT_GE(timeouts.size(), 64U);
    EXPECT_EQ(timeouts[10], 2000ms);
    EXPECT_THAT(std::vector<std::chrono::milliseconds>(timeouts.begin() + 11, timeouts.begin() + 61), Each(200ms));
}

// A whole-image script includes both edge chunks with address-sensitive data;
// N83M's nonzero image base must be subtracted exactly once.
TEST_P(SubaruDensoSh72543CanDieselExecutorWrite, CloseRetriesSettleAndChecksumOutcomes)
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
    ExpectCloseOutcome<SubaruDensoSh72543CanDieselExecutor>(
        *plan, transport, outcome,
        {.sleeps = std::vector<std::chrono::milliseconds>{50ms, 500ms, 100ms},
         .checksum_timeout = 2000ms,
         .reread_timeout = 2000ms});
}

INSTANTIATE_TEST_SUITE_P(ChecksumOutcomes, SubaruDensoSh72543CanDieselExecutorWrite, testing::Range(0, 5),
                         testing::PrintToStringParamName());

TEST(SubaruDensoSh72543CanDieselExecutor, IdentityExchangeFailuresIncludingCancellationAreNonFatal)
{
    fastecu::flash::testing::ExpectIdentityFailuresAreNonFatal<SubaruDensoSh72543CanDieselExecutor>(
        &ReadPlan, {0x22, 0xF1, 0x82}, {200ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms});
}

TEST(SubaruDensoSh72543CanDieselExecutor, InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy)
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
        SubaruDensoSh72543CanDieselExecutor executor;
        const auto plan = ReadPlan();
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(outcome == 1 ? ErrorKind::kDisconnected : ErrorKind::kCancelled));
        EXPECT_TRUE(transport.ScriptConsumed());
        const auto& timeouts = transport.ReadTimeouts();
        const std::size_t count = outcome == 1 ? 11 : 1;
        const std::size_t trailing_read_count = outcome == 1 ? 3 : 0;
        ASSERT_GE(timeouts.size(), count + trailing_read_count);
        using TimeoutDifference = std::vector<std::chrono::milliseconds>::difference_type;
        const auto end = timeouts.end() - static_cast<TimeoutDifference>(trailing_read_count);
        EXPECT_THAT(std::vector<std::chrono::milliseconds>(end - static_cast<TimeoutDifference>(count), end),
                    Each(2000ms));
        std::vector<std::chrono::milliseconds> sleeps{500ms};
        if (outcome == 1)
        {
            sleeps.insert(sleeps.end(), 10, 100ms);
        }
        EXPECT_EQ(clock.sleep_calls, sleeps);
    }
}
} // namespace
