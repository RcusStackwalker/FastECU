#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string_view>
#include <vector>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
#include "src/backend/flash/flash_validation.h"
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
using fastecu::testing::IsOk;

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
    bytes::Bytes out;
    for (const char c : text)
    {
        out.push_back(static_cast<bytes::Byte>(c));
    }
    return out;
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

FlashPlan ReadPlan()
{
    auto plan = BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt, std::nullopt);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
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
    const auto setup = SubaruDensoMc68hc16y5_02BdmExecutor{}.TransportSetup(ReadPlan());
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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& image = *result->read_bytes;
    ASSERT_EQ(image.size(), 0x30000U);
    for (const std::uint32_t address : PageAddresses())
    {
        ASSERT_TRUE(std::equal(image.begin() + address, image.begin() + address + 0x400, PageBytes(address).begin()))
            << std::format("page 0x{:05X}", address);
    }
    EXPECT_TRUE(
        std::all_of(image.begin() + 0x20000, image.begin() + 0x28000, [](bytes::Byte value) { return value == 0xff; }));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.framed_calls, 0);
    // Every read (the initial discard and each page's polls) uses the
    // legacy short timeout; FakeClock does not advance on reads, so a
    // budget's full value is what gets recorded each time.
    EXPECT_TRUE(std::ranges::all_of(transport.read_timeouts, [](auto timeout) { return timeout == 200ms; }));
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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    const auto& read_bytes = result->read_bytes;
    ASSERT_TRUE(read_bytes.has_value());
    EXPECT_TRUE(std::equal(read_bytes->begin(), read_bytes->begin() + 0x400, PageBytes(0).begin()));
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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

// 40 bytes 0x01..0x28, padded by the plan to two 32-byte chunks.
bytes::Bytes KernelBytes()
{
    bytes::Bytes kernel;
    for (int value = 1; value <= 40; ++value)
    {
        kernel.push_back(static_cast<bytes::Byte>(value));
    }
    return kernel;
}

FlashPlan WritePlan()
{
    auto plan = BuildSubaruDensoMc68hc16y502BdmPlan(
        FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
        KernelImage{.id = "bdm-kernel", .load_address = 0x20000, .bytes = KernelBytes()});
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
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
std::size_t ScriptBootstrap(ScriptedKlineFlashTransport& transport, Gate broken = Gate::kNone)
{
    const bytes::Bytes padded = WritePlan().ImageOrEmpty();
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
    const std::size_t writes = ScriptBootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), writes);
    EXPECT_EQ(transport.framed_calls, 0);
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{2, 2}));
    EXPECT_TRUE(
        std::ranges::any_of(events.logs, [](const auto& entry) { return entry.second == "BDM wpcsp reply: 3f 3f "; }));
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
    const bytes::Bytes padded = WritePlan().ImageOrEmpty();
    transport.ExpectRawWrite(bytes::ByteView(padded).first(0x20));
    transport.QueueRawRead(Ascii("NAK"));
    Nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

    // The split ACK_CMD_WDMEM passed: the executor reached the first chunk,
    // whose short "NAK" then times out.
    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AWrongAcknowledgementAtAnyGateStopsTheUpload)
{
    for (const Gate gate : {Gate::kUploadCommand, Gate::kFirstChunk, Gate::kScibCommand, Gate::kScibValue})
    {
        ScriptedKlineFlashTransport transport;
        const std::size_t writes = ScriptBootstrap(transport, gate);
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenChunksStopsBeforeTheNextChunk)
{
    ScriptedKlineFlashTransport transport;
    Nothing(transport);
    transport.ExpectRawWrite(Ascii("wdmem 0x00020000 0x00000040"));
    transport.QueueRawRead(Ascii("ACK_CMD_WDMEM"));
    transport.ExpectRawWrite(bytes::ByteView(WritePlan().ImageOrEmpty()).first(0x20));
    transport.QueueRawRead(Ascii("ACK_WR"));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    // Trips once the first chunk's progress has been reported, so the chunk's
    // own ack read completes normally and only the executor's own check
    // between chunks can stop the next upload.
    cancellation.SetPredicate([&events] { return !events.progress_calls.empty(); });

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationAfterGoStillSucceeds)
{
    ScriptedKlineFlashTransport transport;
    ScriptBootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    // Trips as soon as `go` has been written: the kernel is already running.
    cancellation.SetPredicate([&transport] { return transport.WritesConsumed() >= 7; });
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

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

        auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
        EXPECT_EQ(transport.WritesConsumed(), 0U);
        EXPECT_TRUE(events.progress_calls.empty());
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

        auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
        EXPECT_EQ(transport.WritesConsumed(), 0U);
        EXPECT_TRUE(events.progress_calls.empty());
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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_TRUE(events.progress_calls.empty());
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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

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

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorOnWpcspReplyPropagates)
{
    // Scripts the full upload and SCIB sequence, then fails the very first
    // wpcsp reply read: write_mem() :296-305's ungated replies still
    // propagate a genuine transport error unchanged.
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError, /*fail_after_reads=*/9};
    const bytes::Bytes padded = WritePlan().ImageOrEmpty();
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

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 6U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorOnGoReplyStillSucceedsWithAWarning)
{
    // Allows every raw read through both wpcsp replies to succeed, then fails
    // the go reply read: write_mem() :317-323 still returns success, logging
    // a warning instead of the reply.
    FaultingTransport transport{FaultingTransport::Fault::kRawReadError, /*fail_after_reads=*/12};
    ScriptBootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(WritePlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(std::ranges::any_of(
        events.logs, [](const auto& entry)
        { return entry.first == LogLevel::kWarning && entry.second.starts_with("BDM go reply not read"); }));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, DiscardLogsAnyBytesItDrainsAtDebugLevel)
{
    RawOnlyTransport transport;
    transport.QueueRawRead(bytes::Bytes{0xde, 0xad}); // stale bytes buffered from a prior session
    ScriptRead(transport);                            // its leading empty read ends the drain
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.Execute(ReadPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(
        std::ranges::any_of(events.logs, [](const auto& entry)
                            { return entry.first == LogLevel::kDebug && entry.second == "BDM discarded: de ad "; }));
}
} // namespace
} // namespace fastecu::flash
