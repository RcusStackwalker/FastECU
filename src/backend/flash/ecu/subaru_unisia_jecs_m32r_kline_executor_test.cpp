#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
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
using Line = ScriptedKlineFlashTransport::ControlLineAction;

constexpr std::string_view kProtocol = "sub_ecu_unisia_jecs_20";
constexpr std::string_view kMcu = "M32R_128KB";
constexpr std::uint32_t kRomSize = 0x20000;
constexpr std::string_view kEcuId = "123456789A";

// Built independently of SsmProtocol so the tests pin the wire bytes.
bytes::Bytes request(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0x10, 0xf0, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::sum8(frame));
    return frame;
}

bytes::Bytes reply(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::sum8(frame));
    return frame;
}

// FF, three capability bytes, the five ECU ID bytes at frame offset 8, then
// trailing capability bytes legacy discarded (read_mem() :162-163).
bytes::Bytes init_reply()
{
    return reply({0xff, 0xa1, 0xa2, 0xa3, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xb1, 0xb2});
}

bytes::Bytes page_data(std::uint32_t page)
{
    return bytes::Bytes(0x80, static_cast<bytes::Byte>(page * 7 + 1));
}

bytes::Bytes page_request(std::uint32_t address)
{
    return request({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                    static_cast<bytes::Byte>(address), 0x7f});
}

bytes::Bytes page_reply(std::uint32_t page)
{
    bytes::Bytes payload{0xe0};
    const bytes::Bytes data = page_data(page);
    payload.insert(payload.end(), data.begin(), data.end());
    return reply(payload);
}

void script_warm_entry(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("read-mode probe answered");
    transport.exchange(request({0xbf}), init_reply());
}

void script_cold_entry(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("cold init");
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xb8, 0x00, 0x00, 0x00, 0x75}), reply({0xf8, 0x75}));
    transport.exchange(request({0xbf}), init_reply());
}

void script_pages(ScriptedKlineFlashTransport& transport, std::uint32_t count)
{
    auto section = transport.section("A0 pages");
    for (std::uint32_t page = 0; page < count; ++page)
    {
        transport.exchange(page_request(0x100000 + page * 0x80), page_reply(page));
    }
}

FlashPlan read_plan(std::string_view protocol = kProtocol, std::string_view mcu = kMcu)
{
    auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt, false);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

struct RunContext
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> run(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, RunContext& context)
{
    return SubaruUnisiaJecsM32rKlineExecutor{}.execute(plan, transport, context.clock, context.cancellation,
                                                       context.events);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TransportSetupIs4800BaudPlainSsm)
{
    const auto setup = SubaruUnisiaJecsM32rKlineExecutor{}.transport_setup(read_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 4800);      // execute() :57
    EXPECT_FALSE(setup->iso14230);     // execute() :51
    EXPECT_EQ(setup->tester_id, 0xf0); // execute() :55
    EXPECT_EQ(setup->target_id, 0x10); // execute() :56
    EXPECT_EQ(setup->parity, KlineParity::None);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BeforeConfigureClearsTheIso14230Header)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rKlineExecutor{}.before_transport_configure(transport, clock, cancellation), IsOk());
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
    EXPECT_TRUE(transport.lifecycle_calls_.empty());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadsTheRomWhenAlreadyInReadMode)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, kRomSize / 0x80);
    RunContext context;

    auto result = run(read_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kRomSize);
    for (std::uint32_t page = 0; page < kRomSize / 0x80; ++page)
    {
        const auto offset = static_cast<std::ptrdiff_t>(page) * 0x80;
        ASSERT_TRUE(std::equal(result->read_bytes->begin() + offset, result->read_bytes->begin() + offset + 0x80,
                               page_data(page).begin()))
            << page;
    }
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, std::vector<int>{38400});
    EXPECT_TRUE(transport.control_line_trace_.empty()) << "legacy read touched no LEC line";
    EXPECT_EQ(transport.read_timeouts_.front(), 2000ms);
    EXPECT_EQ(transport.read_timeouts_.back(), 3000ms);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{1024, 1024}));
    EXPECT_EQ(context.clock.elapsed(), 1024 * 1ms); // read_mem() :314
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitSwitchesBaudBeforeReading)
{
    ScriptedKlineFlashTransport transport;
    script_cold_entry(transport);
    script_pages(transport, kRomSize / 0x80);
    RunContext context;

    auto result = run(read_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{38400, 4800, 38400}));
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TruncatedProbeReplyFallsBackToColdInit)
{
    ScriptedKlineFlashTransport transport;
    // A valid FF frame too short to carry the five ID bytes.
    transport.exchange(request({0xbf}), reply({0xff, 0xa1}));
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xb8, 0x00, 0x00, 0x00, 0x75}), reply({0xf8, 0x75}));
    transport.exchange(request({0xbf}), init_reply());
    script_pages(transport, kRomSize / 0x80);
    RunContext context;

    auto result = run(read_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitRejectsABadBaudChangeReply)
{
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xb8, 0x00, 0x00, 0x00, 0x75}), reply({0x7f, 0xb8, 0x22}));
    RunContext context;

    auto result = run(read_plan(), transport, context);

    EXPECT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{38400, 4800}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitWithoutAnyReplyTimesOut)
{
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitReplyTooShortForTheEcuIdFails)
{
    // expect_ssm_init() gates the cold BF reply on carrying the full five-byte
    // ECU ID; a valid FF frame that is too short is rejected rather than
    // accepted with whatever bytes were present (read_mem() :162-163).
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), reply({0xff, 0xa1}));
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadFailsWhenTheInitialBaudChangeFails)
{
    ScriptedKlineFlashTransport transport;
    transport.set_baud_result_ = fail(ErrorKind::Disconnected, "adapter gone");
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ShortPageFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    bytes::Bytes short_page{0xe0};
    short_page.resize(0x80, 0x11); // SID + 127 data bytes
    transport.exchange(page_request(0x100000), reply(short_page));
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BadChecksumPageFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    bytes::Bytes corrupted = page_reply(0);
    corrupted.back() ^= 0x01U;
    transport.exchange(page_request(0x100000), corrupted);
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, NegativePageReplyFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    transport.exchange(page_request(0x100000), reply({0x7f, 0xa0, 0x31}));
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, MissingPageTimesOut)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    transport.expectWrite(page_request(0x100000));
    transport.queue_no_frame();
    RunContext context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationStopsTheReadBeforeTheNextPage)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, 2);
    RunContext context;
    // Trips once the probe and the first page request have been written.
    context.cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 2; });

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationBeforeTheFirstWriteAbortsImmediately)
{
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.set_predicate([] { return true; });

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationAfterTheFirstReadAbortsBeforeInspectingTheReply)
{
    ScriptedKlineFlashTransport transport;
    transport.exchange(request({0xbf}), init_reply());
    RunContext context;
    // Trips once the probe reply has actually been read, not before.
    context.cancellation.set_predicate([&transport] { return !transport.read_timeouts_.empty(); });

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationBeforeAPageRequestPropagatesThroughExchangeExpect)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    RunContext context;
    // The probe's send and receive each check cancellation once (calls 1-2);
    // trip on the third check, which is the first page request's send.
    context.cancellation.cancel_on_check(3);

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

class SubaruUnisiaJecsM32rKlineReadSizes
    : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(SubaruUnisiaJecsM32rKlineReadSizes, ReadsEveryPageOfTheVariant)
{
    const auto& [protocol, mcu, rom_size] = GetParam();
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, rom_size / 0x80);
    RunContext context;

    auto result = run(read_plan(protocol, mcu), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->read_bytes->size(), rom_size);
    EXPECT_TRUE(transport.scriptConsumed());
}

INSTANTIATE_TEST_SUITE_P(AllVariants, SubaruUnisiaJecsM32rKlineReadSizes,
                         ::testing::Values(std::tuple{"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000U},
                                           std::tuple{"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000U},
                                           std::tuple{"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000U},
                                           std::tuple{"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000U}));

const bytes::Bytes& rom_image()
{
    static const bytes::Bytes image = []
    {
        bytes::Bytes out(kRomSize);
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<bytes::Byte>(i * 3 + 1);
        }
        return out;
    }();
    return image;
}

FlashPlan write_plan()
{
    auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Write, kProtocol, kMcu, rom_image(), false);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

bytes::Bytes block_request(std::uint32_t index)
{
    const std::uint32_t address = index * 0x80;
    const bool last = index == kRomSize / 0x80 - 1;
    bytes::Bytes payload{0xaf, static_cast<bytes::Byte>(last ? 0x69 : 0x61), static_cast<bytes::Byte>(address >> 16U),
                         static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address)};
    for (std::uint32_t j = 0; j < 0x80; ++j)
    {
        payload.push_back(static_cast<bytes::Byte>(rom_image()[address + j] ^ 0x82U));
    }
    return request(payload);
}

const bytes::Bytes kEraseStarted = reply({0xef, 0x42});
const bytes::Bytes kDone = reply({0xef, 0x52});

void script_obk_running(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("OBK probe answered");
    transport.exchange(request({0xaf}), reply({0xef}));
}

void script_cold_flash_mode(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("enter flash mode");
    transport.expectWrite(request({0xaf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}), reply({0xef}));
}

// One empty poll before each erase reply, then the trailing read.
void script_erase(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("erase");
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queue_no_frame();
    transport.queueRead(kEraseStarted);
    transport.queue_no_frame();
    transport.queueRead(kDone);
    transport.queue_no_frame();
}

void script_blocks(ScriptedKlineFlashTransport& transport, std::uint32_t count)
{
    auto section = transport.section("program blocks");
    for (std::uint32_t index = 0; index < count; ++index)
    {
        transport.exchange(block_request(index), kDone);
    }
}

std::uint32_t block_count()
{
    return kRomSize / 0x80;
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WritesWhenTheKernelIsAlreadyRunning)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count());
    RunContext context;

    auto result = run(write_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_FALSE(result->rom_id.has_value()) << "legacy set RomId only on read";
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, std::vector<int>{19200});
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<Line>{Line::EnableProgrammingVoltageLine, Line::DisableLecLines}));
    // After the OBK probe, before AF 31 (write_mem() :441-444).
    EXPECT_EQ(transport.programming_voltage_line_write_index_, std::optional<std::size_t>(1));
    // One 500 ms sleep after each empty erase poll.
    EXPECT_EQ(context.clock.elapsed(), 1000ms);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{1024, 1024}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EntersFlashModeFromCold)
{
    ScriptedKlineFlashTransport transport;
    script_cold_flash_mode(transport);
    script_erase(transport);
    script_blocks(transport, block_count());
    RunContext context;

    ASSERT_THAT(run(write_plan(), transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{19200, 4800, 19200}));
    EXPECT_EQ(transport.programming_voltage_line_write_index_, std::optional<std::size_t>(3));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, RejectedFlashModeEntryNeverRaisesProgrammingVoltage)
{
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}),
                       reply({0x7f, 0xaf, 0x22}));
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace_, std::vector<Line>{Line::DisableLecLines});
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WriteFailsWhenTheInitialBaudChangeFails)
{
    ScriptedKlineFlashTransport transport;
    transport.set_baud_result_ = fail(ErrorKind::Disconnected, "adapter gone");
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
    // execute() :71-72 still drops the LEC line on this early exit.
    EXPECT_EQ(transport.control_line_trace_, std::vector<Line>{Line::DisableLecLines});
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationBeforeRaisingProgrammingVoltageAbortsTheWrite)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    RunContext context;
    // The OBK probe's send and receive each check cancellation once (calls
    // 1-2); trip on the third check, which is write_rom()'s own
    // "before programming voltage" gate (write_mem() :440-441).
    context.cancellation.cancel_on_check(3);

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.control_line_trace_, std::vector<Line>{Line::DisableLecLines});
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WriteFailsWhenProgrammingVoltageCannotBeRaised)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.enable_programming_voltage_line_result_ = fail(ErrorKind::Disconnected, "VPP relay stuck");
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<Line>{Line::EnableProgrammingVoltageLine, Line::DisableLecLines}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EraseStartExhaustionFails)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(context.clock.elapsed(), 20 * 500ms);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EraseCompleteExhaustionFailsInsteadOfProgramming)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(kEraseStarted);
    for (int round = 0; round < 40; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 2U) << "legacy went on to program blank flash";
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, UnexpectedEraseReplyFails)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BadBlockReplyStopsBeforeTheNextBlock)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    transport.exchange(block_request(0), reply({0xef, 0x5a}));
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, SilentNonFinalBlockTimesOutBeforeTheNextBlock)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    transport.exchange(block_request(0), kDone);
    transport.expectWrite(block_request(1));
    transport.queue_no_frame();
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 4U);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FinalBlockSilenceSucceedsWithAWarning)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count() - 1);
    transport.expectWrite(block_request(block_count() - 1));
    transport.queue_no_frame();
    RunContext context;

    ASSERT_THAT(run(write_plan(), transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(
        std::ranges::any_of(context.events.logs, [](const auto& log) { return log.first == LogLevel::Warning; }));
    EXPECT_EQ(transport.read_timeouts_.back(), 3000ms);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FinalBlockBadReplyFails)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count() - 1);
    transport.exchange(block_request(block_count() - 1), reply({0x7f, 0xaf, 0x22}));
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationDuringProgrammingReportsCancelledAndDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, 2);
    RunContext context;
    // Trips as soon as the first block has been written.
    context.cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 3; });

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationDuringTheErasePollDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;
    // Trips on the third erase poll.
    context.cancellation.set_predicate([&transport] { return transport.read_timeouts_.size() >= 4; });

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<Line>{Line::EnableProgrammingVoltageLine, Line::DisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TransportErrorMidProgrammingPropagatesAndDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    transport.expectWrite(block_request(0));
    transport.queue_error(ErrorKind::Disconnected, "adapter unplugged");
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CleanupFailureFailsAnOtherwiseSuccessfulWrite)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count());
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "RTS stuck");
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Disconnected));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CleanupFailureNeverReplacesAnEarlierError)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "RTS stuck");
    RunContext context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
}
} // namespace
} // namespace fastecu::flash
