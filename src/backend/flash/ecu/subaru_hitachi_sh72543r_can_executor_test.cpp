#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>
#include <functional>
#include <limits>

namespace
{
using namespace fastecu;
using namespace fastecu::flash;
using namespace std::chrono_literals;
using bytes::Bytes;
Bytes frame(std::uint32_t id, bytes::ByteView payload)
{
    Bytes out;
    bytes::appendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
Bytes req(bytes::ByteView b)
{
    return frame(0x7e0, b);
}
Bytes reply(bytes::ByteView b)
{
    return frame(0x7e8, b);
}
// Test-only boundary instrumentation: errors/cancellation occur at real I/O boundaries.
class Transport : public ScriptedCanFlashTransport
{
  public:
    std::function<void(std::size_t)> after_read;
    std::optional<std::size_t> fail_write;
    std::optional<std::size_t> fail_read;
    ErrorKind injected_error = ErrorKind::Disconnected;
    std::size_t reads = 0;
    std::size_t writes = 0;
    Status write(bytes::ByteView b, const ICancellationToken& c) override
    {
        if (fail_write == writes++)
            return fail(injected_error, "injected write failure");
        return ScriptedCanFlashTransport::write(b, c);
    }
    Result<std::optional<Bytes>> read(std::chrono::milliseconds timeout, const ICancellationToken& c) override
    {
        if (fail_read == reads)
        {
            ++reads;
            return fail(injected_error, "injected read failure");
        }
        auto result = ScriptedCanFlashTransport::read(timeout, c);
        if (after_read)
            after_read(reads);
        ++reads;
        return result;
    }
};
class Sh72543rExecutor : public ::testing::Test
{
  protected:
    Transport transport;
    FakeClock clock;
    ManualCancellationToken cancel;
    RecordingEventSink events;
    SubaruHitachiSh72543rCanExecutor executor;
    FlashPlan plan(FlashOperation op = FlashOperation::Read, std::string_view protocol = "sub_ecu_hitachi_sh72543r_can")
    {
        auto p = build_subaru_hitachi_sh72543r_can_plan(
            op, protocol, "SH72543R", op == FlashOperation::Read ? std::nullopt : std::optional{Bytes(0x200000)});
        EXPECT_THAT(p, fastecu::testing::IsOk());
        return std::move(*p);
    }
    Result<FlashExecutionResult> run()
    {
        return executor.execute(plan(), transport, clock, cancel, events);
    }
    void x(Bytes request, Bytes response)
    {
        transport.exchange(req(request), reply(response));
    }
    void alive()
    {
        x({0xb7}, {0x7f, 0xb7, 0x13});
    }
    void session(Bytes response = {0x50, 0x03})
    {
        x({0x10, 0x03}, std::move(response));
    }
    void seed(Bytes response = {0x67, 0x01, 0xde, 0xad, 0xbe, 0xef})
    {
        x({0x27, 0x01}, std::move(response));
    }
    void key(Bytes response = {0x67, 0x02})
    {
        // Independent Feistel transcription: DEAD BEEF -> DE44 6FC3, reverse
        // Normal2 key table, legacy operation.cpp:1131-1151 at 5dc86672.
        x({0x27, 0x02, 0xde, 0x44, 0x6f, 0xc3}, std::move(response));
    }
    void access()
    {
        session();
        seed();
        key();
    }
    Bytes pageRequest(std::uint32_t a)
    {
        return {0x23,
                0x24,
                0,
                static_cast<std::uint8_t>(a >> 16),
                static_cast<std::uint8_t>(a >> 8),
                static_cast<std::uint8_t>(a),
                4,
                0};
    }
    Bytes pages()
    {
        Bytes expected;
        expected.reserve(0x200000);
        for (std::uint32_t a = 0; a < 0x200000; a += 0x400)
        {
            Bytes page{0x63};
            for (unsigned i = 0; i < 0x400; ++i)
            {
                auto b = static_cast<std::uint8_t>((a / 0x400 + i) & 255);
                page.push_back(b);
                expected.push_back(b);
            }
            x(pageRequest(a), std::move(page));
        }
        return expected;
    }
    void stop(bool silence = false)
    {
        for (int i = 0; i < (silence ? 6 : 1); ++i)
        {
            transport.expectWrite(req(Bytes{0x10, 1}));
            if (silence)
                transport.queue_no_frame();
            else
                transport.queueRead(reply(Bytes{0x50, 1}));
        }
    }
    void init(Bytes ecu = {0xea, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55}, Bytes cal = {0x49, 4, 0, 'C', 'A', 'L'})
    {
        x({0xb7}, {0x7f, 0xb7, 0x22});
        x({0xaa}, std::move(ecu));
        x({9, 2}, {0x49, 2, 0, 'V', 'I', 'N'});
        x({9, 4}, std::move(cal));
        x({9, 6}, {0x49, 6, 0, 0x12, 0x34});
        x({0xa8, 0, 0, 0, 0xd7}, {});
        x({0xa8, 0, 0, 1, 0x3b}, {});
    }
};
TEST_F(Sh72543rExecutor, SetupAndForeignPlanRejectBeforeIo)
{
    auto cfg = executor.transport_setup(plan());
    ASSERT_THAT(cfg, fastecu::testing::IsOk());
    EXPECT_EQ(cfg->request_id, 0x7e0U);
    EXPECT_EQ(cfg->response_id, 0x7e8U);
    EXPECT_EQ(cfg->bitrate, 500000);
    EXPECT_FALSE(cfg->extended_id);
    auto foreign = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_can",
                                                          "M32R_512KB", std::nullopt);
    ASSERT_THAT(foreign, fastecu::testing::IsOk());
    EXPECT_THAT(executor.transport_setup(*foreign), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(executor.execute(*foreign, transport, clock, cancel, events),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(transport.writes, 0U);
}
TEST_F(Sh72543rExecutor, CompleteReadPinsBytesTimingAndKernelShortcut)
{
    alive();
    access();
    auto expected = pages();
    stop();
    auto result = run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->read_bytes, expected);
    EXPECT_FALSE(result->rom_id);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writes, 2053U);
    EXPECT_EQ(clock.elapsed(), 850ms);
    ASSERT_EQ(transport.readTimeouts().size(), 2053U);
    EXPECT_EQ(transport.readTimeouts().front(), 200ms);
    EXPECT_EQ(transport.readTimeouts().back(), 800ms);
    for (std::size_t i = 1; i + 1 < transport.readTimeouts().size(); ++i)
        EXPECT_EQ(transport.readTimeouts()[i], 2000ms);
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.back().done, events.phase_progress_calls.back().total);
}
TEST_F(Sh72543rExecutor, FullInitializationReturnsIdentityAndRecoveryAlias)
{
    init();
    access();
    auto expected = pages();
    stop();
    auto result = executor.execute(plan(FlashOperation::Read, "sub_ecu_hitachi_sh72543r_can_recovery"), transport,
                                   clock, cancel, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, "CAL_1122334455_");
    EXPECT_EQ(result->read_bytes, expected);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(clock.elapsed(), 1450ms);
}
TEST_F(Sh72543rExecutor, PartialIdentityDoesNotReadPastReply)
{
    init({0xea}, {0x49, 4, 0, 'C', 'A', 'L'});
    access();
    auto expected = pages();
    stop(true);
    auto result = run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, "CAL_");
    EXPECT_EQ(result->read_bytes, expected);
    EXPECT_TRUE(transport.scriptConsumed());
}
TEST_F(Sh72543rExecutor, ToleratedSessionAndMissingIdentityStillRead)
{
    init({0xea}, {0x49, 4});
    session({0x7f, 0x10, 0x22});
    seed();
    key();
    auto expected = pages();
    stop();
    auto result = run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->rom_id);
    EXPECT_EQ(result->read_bytes, expected);
}
TEST_F(Sh72543rExecutor, ShortSeedStopsBeforeKey)
{
    for (unsigned count = 0; count < 4; ++count)
    {
        Transport t;
        t.exchange(req(Bytes{0xb7}), reply(Bytes{0x7f, 0xb7, 0x13}));
        t.exchange(req(Bytes{0x10, 3}), reply(Bytes{0x50, 3}));
        Bytes s{0x67, 1};
        s.resize(2 + count, 0xde);
        t.exchange(req(Bytes{0x27, 1}), reply(s));
        EXPECT_THAT(executor.execute(plan(), t, clock, cancel, events),
                    fastecu::testing::IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(t.scriptConsumed());
        EXPECT_EQ(t.writes, 3U);
    }
}
TEST_F(Sh72543rExecutor, WrongKeyStopsBeforePage)
{
    alive();
    session();
    seed();
    key({0x67, 3});
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}
TEST_F(Sh72543rExecutor, RejectsMalformedPagesWithoutPartialSuccess)
{
    for (auto size : {0U, 1U, 0x400U, 0x402U})
    {
        Transport t;
        t.exchange(req(Bytes{0xb7}), reply(Bytes{0x7f, 0xb7, 0x13}));
        t.exchange(req(Bytes{0x10, 3}), reply(Bytes{0x50, 3}));
        t.exchange(req(Bytes{0x27, 1}), reply(Bytes{0x67, 1, 0xde, 0xad, 0xbe, 0xef}));
        t.exchange(req(Bytes{0x27, 2, 0xde, 0x44, 0x6f, 0xc3}), reply(Bytes{0x67, 2}));
        Bytes p(size, 0x63);
        t.exchange(req(pageRequest(0)), reply(p));
        EXPECT_THAT(executor.execute(plan(), t, clock, cancel, events),
                    fastecu::testing::IsErr(ErrorKind::BadResponse));
        EXPECT_EQ(t.writes, 5U);
        EXPECT_TRUE(t.scriptConsumed());
    }
}
TEST_F(Sh72543rExecutor, WrongPageServiceIsRejected)
{
    alive();
    access();
    x(pageRequest(0), Bytes(0x401, 0xf7));
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}
TEST_F(Sh72543rExecutor, RequiredPageTimeoutIsFatal)
{
    alive();
    access();
    transport.expectWrite(req(pageRequest(0)));
    transport.queue_no_frame();
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}
TEST_F(Sh72543rExecutor, CancellationAfterReadStopsNextCommand)
{
    alive();
    access();
    transport.after_read = [&](std::size_t n)
    {
        if (n == 3)
            cancel.cancel();
    };
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writes, 4U);
}
TEST_F(Sh72543rExecutor, DisconnectAndFailedWritesAreFatal)
{
    transport.fail_write = 0;
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::Disconnected));
    transport.fail_write.reset();
    transport.fail_read = 0;
    alive();
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::Disconnected));
}
TEST_F(Sh72543rExecutor, CancelledBeforeStartDoesNoIo)
{
    cancel.cancel();
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writes, 0U);
}
TEST_F(Sh72543rExecutor, CancellationDuringPagesDoesNotPublishPartialImage)
{
    alive();
    access();
    pages();
    stop();
    transport.after_read = [&](std::size_t n)
    {
        if (n == 100)
            cancel.cancel();
    };
    EXPECT_THAT(run(), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writes, 101U);
    EXPECT_LT(events.phase_progress_calls.back().done, events.phase_progress_calls.back().total);
}
TEST_F(Sh72543rExecutor, EcuOnlyIdentityAndExplicitOptionalTimeoutArePreserved)
{
    init({0xea, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55}, {0x49, 4});
    access();
    pages();
    stop();
    auto result = run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, "1122334455_");
}
TEST_F(Sh72543rExecutor, ExplicitTimeoutDuringOptionalSessionIsTolerated)
{
    alive();
    transport.expectWrite(req(Bytes{0x10, 3}));
    transport.queue_error(ErrorKind::Timeout);
    seed();
    key();
    pages();
    stop();
    EXPECT_THAT(run(), fastecu::testing::IsOk());
}
TEST_F(Sh72543rExecutor, ForgedDryRunAndWireChangesAreRejectedBeforeIo)
{
    for (int mutation = 0; mutation < 2; ++mutation)
    {
        FlashPlanFields f{.operation = FlashOperation::TestWrite,
                          .family = FlashFamily::SubaruHitachiSh72543rCan,
                          .transport = TransportKind::CanIso15765,
                          .target_id = "sub_ecu_hitachi_sh72543r_can",
                          .mcu_name = "SH72543R",
                          .transfer_region = {0x6000, 0x1fa000},
                          .erase_regions = {{0x6000, 0x1fa000}},
                          .image = Bytes(0x200000),
                          .kernel = std::nullopt,
                          .family_plan = SubaruHitachiSh72543rCanPlan{0x7e0, 0x7e8, 500000, false, 0x400, 0x100},
                          .confirmations = {}};
        if (mutation)
        {
            f.operation = FlashOperation::Write;
            std::get<SubaruHitachiSh72543rCanPlan>(f.family_plan).request_id = 0x7e1;
        }
        auto built = validate_and_build(std::move(f));
        ASSERT_THAT(built, fastecu::testing::IsOk());
        auto error = mutation ? ErrorKind::InvalidConfig : ErrorKind::Unsupported;
        EXPECT_THAT(executor.transport_setup(*built), fastecu::testing::IsErr(error));
        EXPECT_THAT(executor.execute(*built, transport, clock, cancel, events), fastecu::testing::IsErr(error));
        EXPECT_EQ(transport.writes, 0U);
    }
}

} // namespace
