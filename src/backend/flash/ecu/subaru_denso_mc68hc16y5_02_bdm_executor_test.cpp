#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
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
    return bytes::Bytes(0x400, static_cast<bytes::Byte>((address >> 10) ^ 0x5a));
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
    // Trips once the buffer clear and the first page have been read.
    cancellation.set_predicate([&transport] { return transport.read_timeouts_.size() >= 2; });
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}
} // namespace
} // namespace fastecu::flash
