#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string_view>
#include <vector>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
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
    Result<std::size_t> write(bytes::ByteView data) override
    {
        ++framed_calls;
        return ScriptedKlineFlashTransport::write(data);
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        ++framed_calls;
        return ScriptedKlineFlashTransport::read(timeout, cancellation);
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
        RawWriteError,
        RawWriteShort,
        RawReadError,
    };

    explicit FaultingTransport(Fault fault, int fail_after_reads = 0)
        : fault_(fault), fail_after_reads_(fail_after_reads)
    {
    }

    Result<std::size_t> write_raw(bytes::ByteView data) override
    {
        if (fault_ == Fault::RawWriteError)
        {
            return fail(ErrorKind::Disconnected, "injected raw write failure");
        }
        if (fault_ == Fault::RawWriteShort)
        {
            return data.size() - 1U;
        }
        return ScriptedKlineFlashTransport::write_raw(data);
    }
    Result<OptionalBytes> read_raw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (fault_ == Fault::RawReadError)
        {
            if (reads_seen_ >= fail_after_reads_)
            {
                return fail(ErrorKind::Timeout, "injected raw read failure");
            }
            ++reads_seen_;
        }
        return ScriptedKlineFlashTransport::read_raw(timeout, cancellation);
    }

  private:
    Fault fault_;
    int fail_after_reads_;
    int reads_seen_ = 0;
};

bytes::Bytes ascii(std::string_view text)
{
    bytes::Bytes out;
    for (const char c : text)
    {
        out.push_back(static_cast<bytes::Byte>(c));
    }
    return out;
}

void nothing(ScriptedKlineFlashTransport& transport)
{
    transport.queueRawRead(bytes::Bytes{});
}

std::vector<std::uint32_t> page_addresses()
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

bytes::Bytes page_bytes(std::uint32_t address)
{
    return bytes::Bytes(0x400, static_cast<bytes::Byte>((address >> 10U) ^ 0x5aU));
}

bytes::Bytes rpmem(std::uint32_t address)
{
    return ascii(std::format("rpmem 0x{:08X} 0x00000400", address));
}

FlashPlan read_plan()
{
    auto plan =
        build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt, std::nullopt);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// Scripts the full 160-page read; when `split_first_page` is set the first
// page arrives in two separate polls, 0x100 bytes then 0x300 bytes, with an
// empty read ending the first poll. Legacy replaced its buffer on each poll,
// so only accumulation across polls yields the whole page.
void script_read(ScriptedKlineFlashTransport& transport, bool split_first_page = false)
{
    nothing(transport); // read_mem() :113 clears the receive buffer
    for (const std::uint32_t address : page_addresses())
    {
        transport.expectRawWrite(rpmem(address));
        const bytes::Bytes page = page_bytes(address);
        if (split_first_page && address == 0)
        {
            transport.queueRawRead(bytes::ByteView(page).first(0x100));
            nothing(transport);
            transport.queueRawRead(bytes::ByteView(page).subspan(0x100));
        }
        else
        {
            transport.queueRawRead(page);
        }
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportSetupIs115200BaudPlainSerial)
{
    const auto setup = SubaruDensoMc68hc16y5_02BdmExecutor{}.transport_setup(read_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 115200); // execute() :54
    EXPECT_FALSE(setup->iso14230);  // execute() :50
    EXPECT_EQ(setup->tester_id, 0);
    EXPECT_EQ(setup->target_id, 0);
    EXPECT_EQ(setup->parity, KlineParity::None);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, BeforeConfigureClearsTheIso14230Header)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.before_transport_configure(transport, clock, cancellation),
                IsOk());
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ReadsTheAddressSpaceImageWithTheRamHoleFilled)
{
    RawOnlyTransport transport;
    script_read(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& image = *result->read_bytes;
    ASSERT_EQ(image.size(), 0x30000U);
    for (const std::uint32_t address : page_addresses())
    {
        ASSERT_TRUE(std::equal(image.begin() + address, image.begin() + address + 0x400, page_bytes(address).begin()))
            << std::format("page 0x{:05X}", address);
    }
    EXPECT_TRUE(
        std::all_of(image.begin() + 0x20000, image.begin() + 0x28000, [](bytes::Byte value) { return value == 0xff; }));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.framed_calls, 0);
    EXPECT_EQ(transport.read_timeouts_.front(), 200ms);
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{160, 160}));
    // Each page: one 100 ms poll sleep (read_mem() :162) and 1 ms (:204).
    EXPECT_EQ(clock.elapsed(), 160 * 101ms);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AssemblesAPageDeliveredAcrossPolls)
{
    ScriptedKlineFlashTransport transport;
    script_read(transport, true);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(std::equal(result->read_bytes->begin(), result->read_bytes->begin() + 0x400, page_bytes(0).begin()));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ShortPageFailsAfterFiftyPolls)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(rpmem(0));
    transport.queueRawRead(bytes::Bytes(0x10, 0x01));
    for (int poll = 0; poll < 50; ++poll)
    {
        nothing(transport);
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, OverLongPageFailsBeforeTheNextCommand)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(rpmem(0));
    transport.queueRawRead(bytes::Bytes(0x401, 0x01));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenPagesStopsBeforeTheNextCommand)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(rpmem(0));
    transport.queueRawRead(page_bytes(0));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    // Trips once the first page's progress has been reported, so the page's
    // own read completes normally and only the executor's own check between
    // pages can stop the next command.
    cancellation.set_predicate([&events] { return !events.progress_calls.empty(); });

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 1U);
    EXPECT_TRUE(transport.scriptConsumed());
}

// 40 bytes 0x01..0x28, padded by the plan to two 32-byte chunks.
bytes::Bytes kernel_bytes()
{
    bytes::Bytes kernel;
    for (int value = 1; value <= 40; ++value)
    {
        kernel.push_back(static_cast<bytes::Byte>(value));
    }
    return kernel;
}

FlashPlan write_plan()
{
    auto plan = build_subaru_denso_mc68hc16y5_02_bdm_plan(
        FlashOperation::Write, kProtocol, kMcu, std::nullopt,
        KernelImage{.id = "bdm-kernel", .load_address = 0x20000, .bytes = kernel_bytes()});
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

enum class Gate
{
    None,
    UploadCommand, // flash_block() :394
    FirstChunk,    // flash_block() :424
    ScibCommand,   // write_mem() :268
    ScibValue,     // write_mem() :286
};

// Scripts write_mem() and flash_block(). When `broken` names a gate, that
// gate gets a same-length wrong reply and the script stops there. Returns the
// number of writes the executor must have made.
std::size_t script_bootstrap(ScriptedKlineFlashTransport& transport, Gate broken = Gate::None)
{
    const bytes::Bytes padded = *write_plan().image();
    nothing(transport); // write_mem() :226
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    if (broken == Gate::UploadCommand)
    {
        transport.queueRawRead(ascii("ACK_CMD_WPMEM"));
        return 1;
    }
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    for (std::size_t offset = 0; offset < padded.size(); offset += 0x20)
    {
        transport.expectRawWrite(bytes::ByteView(padded).subspan(offset, 0x20));
        if (broken == Gate::FirstChunk)
        {
            transport.queueRawRead(ascii("ACK_XX"));
            return 2;
        }
        transport.queueRawRead(ascii("ACK_WR"));
    }
    nothing(transport); // flash_block() :470
    transport.expectRawWrite(ascii("wdmem 0xFFC28 0x4"));
    if (broken == Gate::ScibCommand)
    {
        transport.queueRawRead(ascii("ACK_CMD_WPMEM"));
        return 4;
    }
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    nothing(transport); // write_mem() :274
    transport.expectRawWrite(bytes::Bytes{0x00, 0x0d, 0x00, 0x0c});
    if (broken == Gate::ScibValue)
    {
        transport.queueRawRead(ascii("ACK_XX"));
        return 5;
    }
    transport.queueRawRead(ascii("ACK_WR"));
    nothing(transport); // write_mem() :293
    transport.expectRawWrite(ascii("wpcsp"));
    transport.queueRawRead(ascii("??"));
    nothing(transport);
    nothing(transport);
    transport.expectRawWrite(ascii("go"));
    nothing(transport);
    return 7;
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, BootstrapsTheKernelWithTheLegacySequence)
{
    RawOnlyTransport transport;
    const std::size_t writes = script_bootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), writes);
    EXPECT_EQ(transport.framed_calls, 0);
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{2, 2}));
    EXPECT_TRUE(
        std::ranges::any_of(events.logs, [](const auto& entry) { return entry.second == "BDM wpcsp reply: 3f 3f "; }));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AssemblesAnAcknowledgementSplitAcrossReads)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    transport.queueRawRead(ascii("ACK_"));
    transport.queueRawRead(ascii("CMD_WDMEM"));
    const bytes::Bytes padded = *write_plan().image();
    transport.expectRawWrite(bytes::ByteView(padded).first(0x20));
    transport.queueRawRead(ascii("NAK"));
    nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    // The split ACK_CMD_WDMEM passed: the executor reached the first chunk,
    // whose short "NAK" then times out.
    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AWrongAcknowledgementAtAnyGateStopsTheUpload)
{
    for (const Gate gate : {Gate::UploadCommand, Gate::FirstChunk, Gate::ScibCommand, Gate::ScibValue})
    {
        ScriptedKlineFlashTransport transport;
        const std::size_t writes = script_bootstrap(transport, gate);
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::BadResponse)) << static_cast<int>(gate);
        EXPECT_EQ(transport.writesConsumed(), writes) << static_cast<int>(gate);
        EXPECT_TRUE(transport.scriptConsumed()) << static_cast<int>(gate);
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ASilentBridgeTimesOut)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenChunksStopsBeforeTheNextChunk)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    transport.expectRawWrite(bytes::ByteView(*write_plan().image()).first(0x20));
    transport.queueRawRead(ascii("ACK_WR"));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    // Trips once the first chunk's progress has been reported, so the chunk's
    // own ack read completes normally and only the executor's own check
    // between chunks can stop the next upload.
    cancellation.set_predicate([&events] { return !events.progress_calls.empty(); });

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationAfterGoStillSucceeds)
{
    ScriptedKlineFlashTransport transport;
    script_bootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    // Trips as soon as `go` has been written: the kernel is already running.
    cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 7; });
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(transport.writesConsumed(), 7U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, RawWriteFailuresDuringReadStopBeforeAnyPageRead)
{
    for (const auto fault : {FaultingTransport::Fault::RawWriteError, FaultingTransport::Fault::RawWriteShort})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        FaultingTransport transport{fault};
        nothing(transport); // read_mem() :113 initial discard succeeds
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::Disconnected));
        EXPECT_EQ(transport.writesConsumed(), 0U);
        EXPECT_TRUE(events.progress_calls.empty());
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, RawWriteFailuresDuringBootstrapStopBeforeAnyAck)
{
    for (const auto fault : {FaultingTransport::Fault::RawWriteError, FaultingTransport::Fault::RawWriteShort})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        FaultingTransport transport{fault};
        nothing(transport); // write_mem() :226 initial discard succeeds
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::Disconnected));
        EXPECT_EQ(transport.writesConsumed(), 0U);
        EXPECT_TRUE(events.progress_calls.empty());
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorMidPagePropagatesUnchanged)
{
    // Allows the initial discard's one raw read to succeed, then fails the
    // very next raw read: the first page's own poll.
    FaultingTransport transport{FaultingTransport::Fault::RawReadError, /*fail_after_reads=*/1};
    nothing(transport); // read_mem() :113 initial discard succeeds
    transport.expectRawWrite(rpmem(0));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 1U);
    EXPECT_TRUE(events.progress_calls.empty());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorMidAckPropagatesUnchanged)
{
    // Allows the initial discard's one raw read to succeed, then fails the
    // very next raw read: the wait for ACK_CMD_WDMEM.
    FaultingTransport transport{FaultingTransport::Fault::RawReadError, /*fail_after_reads=*/1};
    nothing(transport); // write_mem() :226 initial discard succeeds
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, InvalidPlanIsRejectedBeforeAnyTransportIo)
{
    auto foreign_plan =
        build_subaru_unisia_jecs_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_m3779x", "M3779x", std::nullopt);
    ASSERT_THAT(foreign_plan, IsOk());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.transport_setup(*foreign_plan), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(*foreign_plan, transport, clock, cancellation, events),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorOnWpcspReplyPropagates)
{
    // Scripts the full upload and SCIB sequence, then fails the very first
    // wpcsp reply read: write_mem() :296-305's ungated replies still
    // propagate a genuine transport error unchanged.
    FaultingTransport transport{FaultingTransport::Fault::RawReadError, /*fail_after_reads=*/9};
    const bytes::Bytes padded = *write_plan().image();
    nothing(transport); // write_mem() :226
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    for (std::size_t offset = 0; offset < padded.size(); offset += 0x20)
    {
        transport.expectRawWrite(bytes::ByteView(padded).subspan(offset, 0x20));
        transport.queueRawRead(ascii("ACK_WR"));
    }
    nothing(transport); // flash_block() :470
    transport.expectRawWrite(ascii("wdmem 0xFFC28 0x4"));
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    nothing(transport); // write_mem() :274
    transport.expectRawWrite(bytes::Bytes{0x00, 0x0d, 0x00, 0x0c});
    transport.queueRawRead(ascii("ACK_WR"));
    nothing(transport); // write_mem() :293
    transport.expectRawWrite(ascii("wpcsp"));
    // No reply is queued: the injected fault fires on the wpcsp reply read.
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 6U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportErrorOnGoReplyStillSucceedsWithAWarning)
{
    // Allows every raw read through both wpcsp replies to succeed, then fails
    // the go reply read: write_mem() :317-323 still returns success, logging
    // a warning instead of the reply.
    FaultingTransport transport{FaultingTransport::Fault::RawReadError, /*fail_after_reads=*/12};
    script_bootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(std::ranges::any_of(
        events.logs, [](const auto& entry)
        { return entry.first == LogLevel::Warning && entry.second.starts_with("BDM go reply not read"); }));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, DiscardLogsAnyBytesItDrainsAtDebugLevel)
{
    RawOnlyTransport transport;
    transport.queueRawRead(bytes::Bytes{0xde, 0xad}); // stale bytes buffered from a prior session
    script_read(transport);                           // its leading empty read ends the drain
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(
        std::ranges::any_of(events.logs, [](const auto& entry)
                            { return entry.first == LogLevel::Debug && entry.second == "BDM discarded: de ad "; }));
}
} // namespace
} // namespace fastecu::flash
