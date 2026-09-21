#include "src/algorithms/protocol/bytes_compose.h"
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
#include <array>

namespace
{
using namespace bytes::literals;
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
        {
            return fail(injected_error, "injected write failure");
        }
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
        {
            after_read(reads);
        }
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
        return bytes::composeBe(0x23_b, 0x24_b, a, std::uint16_t{1024});
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
            {
                transport.queue_no_frame();
            }
            else
            {
                transport.queueRead(reply(Bytes{0x50, 1}));
            }
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
    {
        EXPECT_EQ(transport.readTimeouts()[i], 2000ms);
    }
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
        {
            cancel.cancel();
        }
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
        {
            cancel.cancel();
        }
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

// Independent test oracle, two 16-bit halves rather than production's packed
// word transform. Literal vectors below were also evaluated in Python from the
// legacy key schedule. Never call the production cipher to form expected frames.
std::uint32_t referenceCipher(std::uint32_t word)
{
    constexpr std::array<unsigned, 32> box{5,  6, 7, 1, 9,  12, 13, 8, 10, 13, 2, 11, 15, 4,  0,  3,
                                           11, 4, 6, 0, 15, 2,  13, 9, 5,  12, 1, 10, 3,  13, 14, 8};
    unsigned left = word >> 16, right = word & 65535;
    for (unsigned round : {0xb740, 0x42da, 0xa7ca, 0x5fb1})
    {
        unsigned index = right ^ round;
        index |= index << 16;
        unsigned f = 0;
        for (int n = 0; n < 4; ++n)
        {
            f |= box[(index >> (4 * n)) & 31] << (4 * n);
        }
        f = ((f >> 3) | (f << 13)) & 65535;
        unsigned next = left ^ f;
        left = right;
        right = next;
    }
    return (right << 16) | left;
}
TEST(Sh72543rCipherOracle, LiteralVectors)
{
    EXPECT_EQ(referenceCipher(0), 0x98a49de7U);
    EXPECT_EQ(referenceCipher(0x00010203), 0x5aef57efU);
    EXPECT_EQ(referenceCipher(0xdeadbeef), 0xfe3eb636U);
    EXPECT_EQ(referenceCipher(0xffffffff), 0x941478d7U);
}
class Sh72543rWrite : public Sh72543rExecutor
{
  protected:
    Bytes image()
    {
        Bytes b;
        b.reserve(0x200000);
        for (std::uint32_t address = 0; address < 0x200000; address += 4)
        {
            bytes::appendU32Be(b, 0xdeadbeefU ^ address);
        }
        return b;
    }
    Result<FlashExecutionResult> write()
    {
        auto p = build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Write, "sub_ecu_hitachi_sh72543r_can",
                                                        "SH72543R", image());
        EXPECT_THAT(p, fastecu::testing::IsOk());
        return executor.execute(*p, transport, clock, cancel, events);
    }
    void setup()
    {
        alive();
        x({0x10, 0x43}, {0x7f, 0x10, 0x22});
        seed();
        key();
        x({0x10, 0x42}, {0x50, 0x42});
        x({0x34, 4, 0x33, 0, 0x60, 0, 0x1f, 0xa0, 0}, {0x74, 0x20});
    }
    void erase(Bytes r = {0x71, 1, 2})
    {
        x({0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}, std::move(r));
    }
    void data(bool silent = false)
    {
        for (std::uint32_t a = 0x6000; a < 0x200000; a += 0x100)
        {
            Bytes payload{0xb6, static_cast<std::uint8_t>(a >> 16), static_cast<std::uint8_t>(a >> 8),
                          static_cast<std::uint8_t>(a)};
            for (unsigned offset = 0; offset < 0x100; offset += 4)
            {
                bytes::appendU32Be(payload, referenceCipher(0xdeadbeefU ^ (a + offset)));
            }
            ASSERT_EQ(payload.size(), 260U);
            transport.expectWrite(req(payload));
            // Legacy does not interpret this content, even negative responses.
            if (silent)
            {
                transport.queue_no_frame();
            }
            else
            {
                transport.queueRead(reply(Bytes{0x7f, 0xb6, 0x22}));
            }
        }
    }
    void finish()
    {
        x({0x37}, {0x77});
        x({0x31, 1, 2, 2, 1}, {0x71, 1, 2});
    }
};
TEST_F(Sh72543rWrite, CompleteWriteUsesAbsoluteOffsetsAndImmediateEraseReply)
{
    setup();
    erase();
    data();
    finish();
    auto result = write();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes);
    EXPECT_FALSE(result->rom_id);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writes, 8105U);
    EXPECT_EQ(transport.reads, 8105U);
    EXPECT_EQ(clock.elapsed(), 82210ms);
    EXPECT_EQ(transport.readTimeouts()[8103], 800ms);
    EXPECT_EQ(transport.readTimeouts()[8104], 2000ms);
    int previousPhase = 0, previousDone = 0;
    for (const auto& p : events.phase_progress_calls)
    {
        EXPECT_GE(p.phase_index, previousPhase);
        if (p.phase_index == previousPhase)
        {
            EXPECT_GE(p.done, previousDone);
        }
        EXPECT_LE(p.done, p.total);
        previousPhase = p.phase_index;
        previousDone = p.done;
    }
    EXPECT_EQ(events.phase_progress_calls.back().done, events.phase_progress_calls.back().total);
}
TEST_F(Sh72543rWrite, DelayedEraseAndSilentDataRepliesStillRequireFinalVerification)
{
    setup();
    erase({0x7f, 0x31, 0x78});
    transport.queue_no_frame();
    transport.queueRead(reply(Bytes{0x71, 1, 2}));
    data(true);
    finish();
    EXPECT_THAT(write(), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.reads, 8107U);
    EXPECT_EQ(clock.elapsed(), 82710ms);
}
class Sh72543rWriteFault : public Sh72543rWrite, public ::testing::WithParamInterface<int>
{
};
TEST_P(Sh72543rWriteFault, EraseExhaustionNeverSendsData)
{
    setup();
    transport.expectWrite(req(Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}));
    for (int i = 0; i < 21; ++i)
    {
        if (GetParam())
        {
            transport.queueRead(reply(Bytes{0x7f, 0x31, 0x78}));
        }
        else
        {
            transport.queue_no_frame();
        }
    }
    EXPECT_THAT(write(), fastecu::testing::IsErr(GetParam() ? ErrorKind::BadResponse : ErrorKind::Timeout));
    EXPECT_EQ(transport.writes, 7U);
    EXPECT_EQ(transport.reads, 27U);
    EXPECT_TRUE(transport.scriptConsumed());
}
INSTANTIATE_TEST_SUITE_P(SilenceAndWrongReply, Sh72543rWriteFault, ::testing::Values(0, 1));
class Sh72543rWriteCancel : public Sh72543rWrite, public ::testing::WithParamInterface<int>
{
};
TEST_P(Sh72543rWriteCancel, CancelAtEveryProgrammingPhaseStopsSubsequentIo)
{
    setup();
    erase();
    data();
    finish();
    transport.after_read = [&](std::size_t n)
    {
        if (n == static_cast<std::size_t>(GetParam()))
        {
            cancel.cancel();
        }
    };
    EXPECT_THAT(write(), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writes, static_cast<std::size_t>(GetParam() + 1));
    EXPECT_LT(events.phase_progress_calls.back().done, events.phase_progress_calls.back().total);
}
// After erase, first/middle/last frame, close, and checksum respectively.
INSTANTIATE_TEST_SUITE_P(Boundaries, Sh72543rWriteCancel, ::testing::Values(6, 7, 100, 8102, 8103, 8104));
TEST_F(Sh72543rWrite, CancelDuringErasePollingStopsBeforeData)
{
    setup();
    erase({0x7f, 0x31, 0x78});
    transport.queue_no_frame();
    transport.after_read = [&](std::size_t n)
    {
        if (n == 7)
        {
            cancel.cancel();
        }
    };
    EXPECT_THAT(write(), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writes, 7U);
}
TEST_F(Sh72543rWrite, DisconnectDuringEraseStopsBeforeData)
{
    setup();
    erase();
    transport.fail_read = 6;
    EXPECT_THAT(write(), fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.writes, 7U);
}
TEST_F(Sh72543rWrite, FailedDataWriteStopsBeforeFinalization)
{
    setup();
    erase();
    data();
    finish();
    transport.fail_write = 7;
    EXPECT_THAT(write(), fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.writes, 8U);
}
TEST_F(Sh72543rWrite, DisconnectedDataReadStopsBeforeFinalization)
{
    setup();
    erase();
    data();
    finish();
    transport.fail_read = 7;
    EXPECT_THAT(write(), fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.writes, 8U);
}
class Sh72543rFinalizeFault : public Sh72543rWrite, public ::testing::WithParamInterface<int>
{
};
TEST_P(Sh72543rFinalizeFault, BoundedFinalizationRetriesClassifyFailure)
{
    setup();
    erase();
    data();
    bool checksum = GetParam() >= 2, response = GetParam() % 2;
    if (checksum)
    {
        x({0x37}, {0x77});
    }
    for (int i = 0; i < 20; ++i)
    {
        transport.expectWrite(req(checksum ? Bytes{0x31, 1, 2, 2, 1} : Bytes{0x37}));
        if (response)
        {
            transport.queueRead(reply(Bytes{0x7f, 0x31, 0x78}));
        }
        else
        {
            transport.queue_no_frame();
        }
    }
    EXPECT_THAT(write(), fastecu::testing::IsErr(response ? ErrorKind::BadResponse : ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writes, static_cast<std::size_t>(8103 + 20 + (checksum ? 1 : 0)));
}
INSTANTIATE_TEST_SUITE_P(CloseAndChecksum, Sh72543rFinalizeFault, ::testing::Values(0, 1, 2, 3));
TEST_F(Sh72543rWrite, LastRetrySucceedsWithoutEarlyCompletion)
{
    setup();
    erase();
    data();
    for (int i = 0; i < 19; ++i)
    {
        x({0x37}, {0x7f, 0x37, 0x78});
    }
    x({0x37}, {0x77});
    for (int i = 0; i < 19; ++i)
    {
        x({0x31, 1, 2, 2, 1}, {0x7f, 0x31, 0x78});
    }
    x({0x31, 1, 2, 2, 1}, {0x71, 1, 2});
    EXPECT_THAT(write(), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST_F(Sh72543rWrite, ReusedExecutorDoesNotLeakReadMetadataIntoWrite)
{
    init();
    access();
    pages();
    stop();
    auto read = run();
    ASSERT_THAT(read, fastecu::testing::IsOk());
    EXPECT_EQ(read->rom_id, "CAL_1122334455_");
    setup();
    erase();
    data();
    finish();
    auto result = write();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->read_bytes);
    EXPECT_FALSE(result->rom_id);
    EXPECT_TRUE(transport.scriptConsumed());
}
} // namespace
