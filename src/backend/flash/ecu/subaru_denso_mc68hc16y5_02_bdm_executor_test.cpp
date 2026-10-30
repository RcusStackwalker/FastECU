#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/faulting_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::_;
using ::testing::Contains;
using ::testing::Each;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::StartsWith;

constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";

// Fails the test on any framed call: every BDM exchange must be raw.
class RawOnlyTransport final : public ScriptedKlineFlashTransport
{
  public:
    Result<std::size_t> Write(bytes::ByteView data) override
    {
        ++framed_calls;
        return ScriptedKlineFlashTransport::Write(data);
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        ++framed_calls;
        return ScriptedKlineFlashTransport::Read(timeout, cancellation);
    }

    int framed_calls = 0;
};

// Injects a fault into every raw write, or into a raw read once a chosen
// number of earlier raw reads have already succeeded normally -- so a test
// can put the failure exactly mid-page or mid-ACK instead of only on the
// very first exchange. Pattern: FaultingTransport in
// subaru_unisia_jecs_executor_test.cpp.
class FaultingTransport final : public ScriptedKlineFlashTransport
{
  public:
    enum class Fault
    {
        kRawWriteError,
        kRawWriteShort,
        kRawReadError,
    };

    explicit FaultingTransport(Fault fault, int fail_after_reads = 0)
        : fault_(fault), fail_after_reads_(fail_after_reads)
    {
    }

    Result<std::size_t> WriteRaw(bytes::ByteView data) override
    {
        if (fault_ == Fault::kRawWriteError)
        {
            return Fail(ErrorKind::kDisconnected, "injected raw write failure");
        }
        if (fault_ == Fault::kRawWriteShort)
        {
            return data.size() - 1U;
        }
        return ScriptedKlineFlashTransport::WriteRaw(data);
    }
    Result<OptionalBytes> ReadRaw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (fault_ == Fault::kRawReadError)
        {
            if (reads_seen_ >= fail_after_reads_)
            {
                return Fail(ErrorKind::kTimeout, "injected raw read failure");
            }
            ++reads_seen_;
        }
        return ScriptedKlineFlashTransport::ReadRaw(timeout, cancellation);
    }

  private:
    Fault fault_;
    int fail_after_reads_;
    int reads_seen_ = 0;
};

bytes::Bytes Ascii(std::string_view text)
{
    return text | std::views::transform([](const char c) { return static_cast<bytes::Byte>(c); }) |
           std::ranges::to<bytes::Bytes>();
}

void Nothing(ScriptedKlineFlashTransport& transport)
{
    transport.QueueRawRead(bytes::Bytes{});
}

std::vector<std::uint32_t> PageAddresses()
{
    std::vector<std::uint32_t> addresses;
    for (std::uint32_t address = 0; address < 0x20000; address += 0x400)
    {
        addresses.push_back(address);
    }
    for (std::uint32_t address = 0x28000; address < 0x30000; address += 0x400)
    {
        addresses.push_back(address);
    }
    return addresses;
}

bytes::Bytes PageBytes(std::uint32_t address)
{
    return bytes::Bytes(0x400, static_cast<bytes::Byte>((address >> 10U) ^ 0x5aU));
}

bytes::Bytes Rpmem(std::uint32_t address)
{
    return Ascii(std::format("rpmem 0x{:08X} 0x00000400", address));
}

Result<FlashPlan> ReadPlan()
{
    return BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt, std::nullopt);
}

// Scripts the full 160-page read; when `split_first_page` is set the first
// page arrives in two separate polls, 0x100 bytes then 0x300 bytes, with an
// empty read ending the first poll. Legacy replaced its buffer on each poll,
// so only accumulation across polls yields the whole page.
void ScriptRead(ScriptedKlineFlashTransport& transport, bool split_first_page = false)
{
    Nothing(transport); // read_mem() :113 clears the receive buffer
    for (const std::uint32_t address : PageAddresses())
    {
        transport.ExpectRawWrite(Rpmem(address));
        const bytes::Bytes page = PageBytes(address);
        if (split_first_page && address == 0)
        {
            transport.QueueRawRead(bytes::ByteView(page).first(0x100));
            Nothing(transport);
            transport.QueueRawRead(bytes::ByteView(page).subspan(0x100));
        }
        else
        {
            transport.QueueRawRead(page);
        }
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportSetupIs115200BaudPlainSerial)
{
    auto built_plan_1 = ReadPlan();
    ASSERT_THAT(built_plan_1, fastecu::testing::IsOk());
    const auto setup = SubaruDensoMc68hc16y5_02BdmExecutor{}.TransportSetup(std::move(*built_plan_1));
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 115200); // execute() :54
    EXPECT_FALSE(setup->iso14230);  // execute() :50
    EXPECT_EQ(setup->tester_id, 0);
    EXPECT_EQ(setup->target_id, 0);
    EXPECT_EQ(setup->parity, KlineParity::kNone);
}

// The bootstrap consent is enforced past the builder: a Write plan assembled
// without it never reaches the adapter.
TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportSetupRejectsAWritePlanWithoutTheBootstrapConsent)
{
    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kWrite,
        .family = FlashFamily::kSubaruDensoMc68hc16y502Bdm,
        .transport = TransportKind::kKline,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcu),
        .transfer_region = MemoryRegion{0x20000, 0x20},
        .erase_regions = {},
        .image = bytes::Bytes(0x20, 0xab),
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200},
        .confirmations = {},
    });
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.TransportSetup(*plan), IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, BeforeConfigureClearsTheIso14230Header)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.BeforeTransportConfigure(transport, clock, cancellation), IsOk());
    EXPECT_EQ(transport.header_mode_calls, std::vector<bool>{false});
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ReadsTheAddressSpaceImageWithTheRamHoleFilled)
{
    RawOnlyTransport transport;
    ScriptRead(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_2 = ReadPlan();
    ASSERT_THAT(built_plan_2, fastecu::testing::IsOk());
    auto result =
        SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_2), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_THAT(result->read_bytes, Optional(_));
    const bytes::Bytes& image = *result->read_bytes;
    ASSERT_EQ(image.size(), 0x30000U);
    for (const std::uint32_t address : PageAddresses())
    {
        SCOPED_TRACE(::testing::PrintToString(address));
        ASSERT_THAT(bytes::ByteView(image).subspan(address, 0x400), test_bytes::BytesEq(PageBytes(address)))
            << std::format("page 0x{:05X}", address);
    }
    EXPECT_THAT(bytes::ByteView(image).subspan(0x20000, 0x8000), test_bytes::BytesFilledWith(0xff));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.framed_calls, 0);
    // Every read (the initial discard and each page's polls) uses the
    // legacy short timeout; FakeClock does not advance on reads, so a
    // budget's full value is what gets recorded each time.
    EXPECT_THAT(transport.read_timeouts, Each(200ms));
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{160, 160}));
    // Each page: one 100 ms poll sleep (read_mem() :162) and 1 ms (:204).
    EXPECT_EQ(clock.Elapsed(), 160 * 101ms);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AssemblesAPageDeliveredAcrossPolls)
{
    ScriptedKlineFlashTransport transport;
    ScriptRead(transport, true);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_3 = ReadPlan();
    ASSERT_THAT(built_plan_3, fastecu::testing::IsOk());
    auto result =
        SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_3), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    const auto& read_bytes = result->read_bytes;
    ASSERT_THAT(read_bytes, Optional(_));
    EXPECT_THAT(bytes::ByteView(*read_bytes).first(0x400), test_bytes::BytesEq(PageBytes(0)));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ShortPageFailsAfterFiftyPolls)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Rpmem(0));
    transport.QueueRawRead(bytes::Bytes(0x10, 0x01));
    for (int poll = 0; poll < 50; ++poll)
    {
        Nothing(transport);
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_4 = ReadPlan();
    ASSERT_THAT(built_plan_4, fastecu::testing::IsOk());
    auto result =
        SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_4), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, OverLongPageFailsBeforeTheNextCommand)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Rpmem(0));
    transport.QueueRawRead(bytes::Bytes(0x401, 0x01));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_5 = ReadPlan();
    ASSERT_THAT(built_plan_5, fastecu::testing::IsOk());
    auto result =
        SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_5), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenPagesStopsBeforeTheNextCommand)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Rpmem(0));
    transport.QueueRawRead(PageBytes(0));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    // Trips once the first page's progress has been reported, so the page's
    // own read completes normally and only the executor's own check between
    // pages can stop the next command.
    cancellation.SetPredicate([&events] { return !events.progress_calls.empty(); });

    auto built_plan_6 = ReadPlan();
    ASSERT_THAT(built_plan_6, fastecu::testing::IsOk());
    auto result =
        SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_6), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

// 40 bytes 0x01..0x28, padded by the plan to two 32-byte chunks.
bytes::Bytes KernelBytes()
{
    return std::views::iota(1, 41) | std::views::transform([](int value) { return static_cast<bytes::Byte>(value); }) |
           std::ranges::to<bytes::Bytes>();
}

Result<FlashPlan> WritePlan()
{
    return BuildSubaruDensoMc68hc16y502BdmPlan(
        FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
        KernelImage{.id = "bdm-kernel", .load_address = 0x20000, .bytes = KernelBytes()});
}

enum class Gate
{
    kNone,
    kUploadCommand, // flash_block() :394
    kFirstChunk,    // flash_block() :424
    kScibCommand,   // write_mem() :268
    kScibValue,     // write_mem() :286
};

// Scripts write_mem() and flash_block(). When `broken` names a gate, that
// gate gets a same-length wrong reply and the script stops there. Returns the
// number of writes the executor must have made.
std::size_t ScriptBootstrap(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, Gate broken = Gate::kNone)
{
    const bytes::Bytes padded = plan.ImageOrEmpty();
    Nothing(transport); // write_mem() :226
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    if (broken == Gate::kUploadCommand)
    {
        transport.QueueRawRead(Ascii("ACK_CMD_WPMEM"));
        return 1;
    }
    transport.QueueRawRead(Ascii("ACK_CMD_WDMEM"));
    for (std::size_t offset = 0; offset < padded.size(); offset += 0x20)
    {
        transport.ExpectRawWrite(bytes::ByteView(padded).subspan(offset, 0x20));
        if (broken == Gate::kFirstChunk)
        {
            transport.QueueRawRead(Ascii("ACK_XX"));
            return 2;
        }
        transport.QueueRawRead(Ascii("ACK_WR"));
    }
    Nothing(transport); // flash_block() :470
    transport.ExpectRawWrite(Ascii("wdmem 0xFFC28 0x4"));
    if (broken == Gate::kScibCommand)
    {
        transport.QueueRawRead(Ascii("ACK_CMD_WPMEM"));
        return 4;
    }
    transport.QueueRawRead(Ascii("ACK_CMD_WDMEM"));
    Nothing(transport); // write_mem() :274
    transport.ExpectRawWrite(bytes::Bytes{0x00, 0x0d, 0x00, 0x0c});
    if (broken == Gate::kScibValue)
    {
        transport.QueueRawRead(Ascii("ACK_XX"));
        return 5;
    }
    transport.QueueRawRead(Ascii("ACK_WR"));
    Nothing(transport); // write_mem() :293
    transport.ExpectRawWrite(Ascii("wpcsp"));
    transport.QueueRawRead(Ascii("??"));
    Nothing(transport);
    Nothing(transport);
    transport.ExpectRawWrite(Ascii("go"));
    Nothing(transport);
    return 7;
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, BootstrapsTheKernelWithTheLegacySequence)
{
    RawOnlyTransport transport;
    auto bootstrap_plan = WritePlan();
    ASSERT_THAT(bootstrap_plan, fastecu::testing::IsOk());
    const std::size_t writes = ScriptBootstrap(*bootstrap_plan, transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_8 = WritePlan();
    ASSERT_THAT(built_plan_8, fastecu::testing::IsOk());
    auto result =
        SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_8), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), writes);
    EXPECT_EQ(transport.framed_calls, 0);
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{2, 2}));
    EXPECT_THAT(events.logs, Contains(Pair(_, "BDM wpcsp reply: 3f 3f ")));
    // 13 reads: discard, WDMEM ACK, 2 chunk ACKs, discard, SCIB-command ACK,
    // 800 ms discard, SCIB-value ACK, discard, wpcsp "??" + empty, empty, go.
    // FakeClock does not advance on reads, so each recorded timeout is the
    // read's full budget, not a shrunk remainder.
    EXPECT_EQ(transport.read_timeouts,
              (std::vector<std::chrono::milliseconds>{200ms, 3000ms, 800ms, 800ms, 200ms, 3000ms, 800ms, 800ms, 200ms,
                                                      800ms, 800ms, 800ms, 800ms}));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AssemblesAnAcknowledgementSplitAcrossReads)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    transport.QueueRawRead(Ascii("ACK_"));
    transport.QueueRawRead(Ascii("CMD_WDMEM"));
    auto built_plan_9 = WritePlan();
    ASSERT_THAT(built_plan_9, fastecu::testing::IsOk());
    const bytes::Bytes padded = std::move(*built_plan_9).ImageOrEmpty();
    transport.ExpectRawWrite(bytes::ByteView(padded).first(0x20));
    transport.QueueRawRead(Ascii("NAK"));
    Nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_10 = WritePlan();
    ASSERT_THAT(built_plan_10, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_10), transport, clock,
                                                                cancellation, events);

    // The split ACK_CMD_WDMEM passed: the executor reached the first chunk,
    // whose short "NAK" then times out.
    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AWrongAcknowledgementAtAnyGateStopsTheUpload)
{
    for (const Gate gate : {Gate::kUploadCommand, Gate::kFirstChunk, Gate::kScibCommand, Gate::kScibValue})
    {
        SCOPED_TRACE(::testing::PrintToString(gate));
        ScriptedKlineFlashTransport transport;
        auto bootstrap_plan = WritePlan();
        ASSERT_THAT(bootstrap_plan, fastecu::testing::IsOk());
        const std::size_t writes = ScriptBootstrap(*bootstrap_plan, transport, gate);
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto built_plan_11 = WritePlan();
        ASSERT_THAT(built_plan_11, fastecu::testing::IsOk());
        auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_11), transport, clock,
                                                                    cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::kBadResponse)) << static_cast<int>(gate);
        EXPECT_EQ(transport.WritesConsumed(), writes) << static_cast<int>(gate);
        EXPECT_TRUE(transport.ScriptConsumed()) << static_cast<int>(gate);
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ASilentBridgeTimesOut)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    Nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_12 = WritePlan();
    ASSERT_THAT(built_plan_12, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_12), transport, clock,
                                                                cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenChunksStopsBeforeTheNextChunk)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    transport.QueueRawRead(Ascii("ACK_CMD_WDMEM"));
    auto built_plan_13 = WritePlan();
    ASSERT_THAT(built_plan_13, fastecu::testing::IsOk());
    transport.ExpectRawWrite(bytes::ByteView(std::move(*built_plan_13).ImageOrEmpty()).first(0x20));
    transport.QueueRawRead(Ascii("ACK_WR"));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    // Trips once the first chunk's progress has been reported, so the chunk's
    // own ack read completes normally and only the executor's own check
    // between chunks can stop the next upload.
    cancellation.SetPredicate([&events] { return !events.progress_calls.empty(); });

    auto built_plan_14 = WritePlan();
    ASSERT_THAT(built_plan_14, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_14), transport, clock,
                                                                cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationAfterGoStillSucceeds)
{
    ScriptedKlineFlashTransport transport;
    auto bootstrap_plan = WritePlan();
    ASSERT_THAT(bootstrap_plan, fastecu::testing::IsOk());
    ScriptBootstrap(*bootstrap_plan, transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    // Trips as soon as `go` has been written: the kernel is already running.
    cancellation.SetPredicate([&transport] { return transport.WritesConsumed() >= 7; });
    RecordingEventSink events;

    auto built_plan_15 = WritePlan();
    ASSERT_THAT(built_plan_15, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_15), transport, clock,
                                                                cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(transport.WritesConsumed(), 7U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, RawWriteFailuresDuringReadStopBeforeAnyPageRead)
{
    for (const auto fault : {FaultingTransport::Fault::kRawWriteError, FaultingTransport::Fault::kRawWriteShort})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        FaultingTransport transport{fault};
        Nothing(transport); // read_mem() :113 initial discard succeeds
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto built_plan_16 = ReadPlan();
        ASSERT_THAT(built_plan_16, fastecu::testing::IsOk());
        auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_16), transport, clock,
                                                                    cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
        EXPECT_EQ(transport.WritesConsumed(), 0U);
        EXPECT_THAT(events.progress_calls, IsEmpty());
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, RawWriteFailuresDuringBootstrapStopBeforeAnyAck)
{
    for (const auto fault : {FaultingTransport::Fault::kRawWriteError, FaultingTransport::Fault::kRawWriteShort})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        FaultingTransport transport{fault};
        Nothing(transport); // write_mem() :226 initial discard succeeds
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto built_plan_17 = WritePlan();
        ASSERT_THAT(built_plan_17, fastecu::testing::IsOk());
        auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_17), transport, clock,
                                                                    cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
        EXPECT_EQ(transport.WritesConsumed(), 0U);
        EXPECT_THAT(events.progress_calls, IsEmpty());
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorMidPagePropagatesUnchanged)
{
    // Allows the initial discard's one raw read to succeed, then fails the
    // very next raw read: the first page's own poll.
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError, /*fail_after_reads=*/1};
    Nothing(transport); // read_mem() :113 initial discard succeeds
    transport.ExpectRawWrite(Rpmem(0));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_18 = ReadPlan();
    ASSERT_THAT(built_plan_18, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_18), transport, clock,
                                                                cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_THAT(events.progress_calls, IsEmpty());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorMidAckPropagatesUnchanged)
{
    // Allows the initial discard's one raw read to succeed, then fails the
    // very next raw read: the wait for ACK_CMD_WDMEM.
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError, /*fail_after_reads=*/1};
    Nothing(transport); // write_mem() :226 initial discard succeeds
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_19 = WritePlan();
    ASSERT_THAT(built_plan_19, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_19), transport, clock,
                                                                cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, InvalidPlanIsRejectedBeforeAnyTransportIo)
{
    auto foreign_plan =
        BuildSubaruUnisiaJecsPlan(FlashOperation::kRead, "sub_ecu_unisia_jecs_m3779x", "M3779x", std::nullopt);
    ASSERT_THAT(foreign_plan, IsOk());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.TransportSetup(*foreign_plan), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(*foreign_plan, transport, clock, cancellation, events),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, PrecancelledRunIsRefusedInAdmission)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation{true};
    RecordingEventSink events;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, IsOk());

    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_THAT(transport.control_line_trace, IsEmpty());
    EXPECT_THAT(events.logs, IsEmpty());
    EXPECT_THAT(events.progress_calls, IsEmpty());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorOnWpcspReplyPropagates)
{
    // Scripts the full upload and SCIB sequence, then fails the very first
    // wpcsp reply read: write_mem() :296-305's ungated replies still
    // propagate a genuine transport error unchanged.
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError, /*fail_after_reads=*/9};
    auto built_plan_20 = WritePlan();
    ASSERT_THAT(built_plan_20, fastecu::testing::IsOk());
    const bytes::Bytes padded = std::move(*built_plan_20).ImageOrEmpty();
    Nothing(transport); // write_mem() :226
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    transport.QueueRawRead(Ascii("ACK_CMD_WDMEM"));
    for (std::size_t offset = 0; offset < padded.size(); offset += 0x20)
    {
        transport.ExpectRawWrite(bytes::ByteView(padded).subspan(offset, 0x20));
        transport.QueueRawRead(Ascii("ACK_WR"));
    }
    Nothing(transport); // flash_block() :470
    transport.ExpectRawWrite(Ascii("wdmem 0xFFC28 0x4"));
    transport.QueueRawRead(Ascii("ACK_CMD_WDMEM"));
    Nothing(transport); // write_mem() :274
    transport.ExpectRawWrite(bytes::Bytes{0x00, 0x0d, 0x00, 0x0c});
    transport.QueueRawRead(Ascii("ACK_WR"));
    Nothing(transport); // write_mem() :293
    transport.ExpectRawWrite(Ascii("wpcsp"));
    // No reply is queued: the injected fault fires on the wpcsp reply read.
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_21 = WritePlan();
    ASSERT_THAT(built_plan_21, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_21), transport, clock,
                                                                cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 6U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorOnGoReplyStillSucceedsWithAWarning)
{
    // Allows every raw read through both wpcsp replies to succeed, then fails
    // the go reply read: write_mem() :317-323 still returns success, logging
    // a warning instead of the reply.
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError, /*fail_after_reads=*/12};
    auto bootstrap_plan = WritePlan();
    ASSERT_THAT(bootstrap_plan, fastecu::testing::IsOk());
    ScriptBootstrap(*bootstrap_plan, transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_22 = WritePlan();
    ASSERT_THAT(built_plan_22, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_22), transport, clock,
                                                                cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kWarning, StartsWith("BDM go reply not read"))));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, DiscardLogsAnyBytesItDrainsAtDebugLevel)
{
    RawOnlyTransport transport;
    transport.QueueRawRead(bytes::Bytes{0xde, 0xad}); // stale bytes buffered from a prior session
    ScriptRead(transport);                            // its leading empty read ends the drain
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto built_plan_23 = ReadPlan();
    ASSERT_THAT(built_plan_23, fastecu::testing::IsOk());
    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(std::move(*built_plan_23), transport, clock,
                                                                cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kDebug, "BDM discarded: de ad ")));
}

// Writes up to and including `go`; once it is sent the kernel is running and
// nothing after it fails the bootstrap.
constexpr std::size_t kWritesThroughGo = 7;

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, EveryRawTransportFaultBeforeGoStopsTheBootstrap)
{
    auto plan = WritePlan();
    ASSERT_THAT(plan, IsOk());
    for (const KlineFaultOp op : {KlineFaultOp::kWriteRaw, KlineFaultOp::kReadRaw})
    {
        SCOPED_TRACE(::testing::Message() << "op " << static_cast<int>(op));
        std::size_t count = 0;
        {
            FaultingKlineFlashTransport transport;
            transport.op = op;
            ScriptBootstrap(*plan, transport);
            FakeClock clock;
            FakeCancellationToken cancellation;
            RecordingEventSink events;
            ASSERT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                        IsOk());
            count = transport.calls;
        }
        ASSERT_GT(count, 0U);
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::Message() << "call " << k);
            FaultingKlineFlashTransport transport;
            transport.op = op;
            transport.fail_at = k;
            ScriptBootstrap(*plan, transport);
            FakeClock clock;
            FakeCancellationToken cancellation;
            RecordingEventSink events;
            const auto result =
                SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(*plan, transport, clock, cancellation, events);
            ASSERT_THAT(transport.writes_at_fault, Optional(_));
            if (*transport.writes_at_fault >= kWritesThroughGo)
            {
                EXPECT_THAT(result, IsOk());
                continue;
            }
            EXPECT_THAT(result, IsErrWith(ErrorKind::kDisconnected, Eq("injected")));
            EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>{transport.WritesConsumed()});
        }
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, EveryCancellationCheckBeforeGoStopsTheBootstrap)
{
    auto plan = WritePlan();
    ASSERT_THAT(plan, IsOk());
    std::size_t checks = 0;
    {
        ScriptedKlineFlashTransport transport;
        ScriptBootstrap(*plan, transport);
        FakeClock clock;
        FakeCancellationToken cancellation;
        cancellation.SetPredicate(
            [&]
            {
                ++checks;
                return false;
            });
        RecordingEventSink events;
        ASSERT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    IsOk());
    }
    ASSERT_GT(checks, 0U);
    for (std::size_t n = 1; n <= checks; ++n)
    {
        SCOPED_TRACE(n);
        ScriptedKlineFlashTransport transport;
        ScriptBootstrap(*plan, transport);
        FakeClock clock;
        FakeCancellationToken cancellation;
        std::size_t seen = 0;
        std::optional<std::size_t> writes_at_cancel;
        cancellation.SetPredicate(
            [&]
            {
                if (++seen == n)
                {
                    writes_at_cancel = transport.WritesConsumed();
                }
                return seen >= n;
            });
        RecordingEventSink events;
        const auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_THAT(writes_at_cancel, Optional(_));
        if (*writes_at_cancel >= kWritesThroughGo)
        {
            EXPECT_THAT(result, IsOk());
            continue;
        }
        EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(writes_at_cancel, std::optional<std::size_t>{transport.WritesConsumed()});
    }
}
} // namespace
} // namespace fastecu::flash
