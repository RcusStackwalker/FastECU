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
Bytes Frame(std::uint32_t id, bytes::ByteView payload)
{
    Bytes out;
    bytes::AppendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
Bytes Req(bytes::ByteView b)
{
    return Frame(0x7e0, b);
}
Bytes Reply(bytes::ByteView b)
{
    return Frame(0x7e8, b);
}
// Test-only boundary instrumentation: errors/cancellation occur at real I/O boundaries.
class Transport : public ScriptedCanFlashTransport
{
  public:
    std::function<void(std::size_t)> after_read;
    std::optional<std::size_t> fail_write;
    std::optional<std::size_t> fail_read;
    ErrorKind injected_error = ErrorKind::kDisconnected;
    std::size_t reads = 0;
    std::size_t writes = 0;
    Status Write(bytes::ByteView b, const ICancellationToken& c) override
    {
        if (fail_write == writes++)
        {
            return Fail(injected_error, "injected write failure");
        }
        return ScriptedCanFlashTransport::Write(b, c);
    }
    Result<std::optional<Bytes>> Read(std::chrono::milliseconds timeout, const ICancellationToken& c) override
    {
        if (fail_read == reads)
        {
            ++reads;
            return Fail(injected_error, "injected read failure");
        }
        auto result = ScriptedCanFlashTransport::Read(timeout, c);
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
    Transport transport_;
    FakeClock clock_;
    ManualCancellationToken cancel_;
    RecordingEventSink events_;
    SubaruHitachiSh72543rCanExecutor executor_;
    FlashPlan Plan(FlashOperation op = FlashOperation::kRead,
                   std::string_view protocol = "sub_ecu_hitachi_sh72543r_can")
    {
        auto p = BuildSubaruHitachiSh72543rCanPlan(
            op, protocol, "SH72543R", op == FlashOperation::kRead ? std::nullopt : std::optional{Bytes(0x200000)});
        EXPECT_THAT(p, fastecu::testing::IsOk());
        return std::move(*p);
    }
    Result<FlashExecutionResult> Run()
    {
        return executor_.Execute(Plan(), transport_, clock_, cancel_, events_);
    }
    void X(Bytes request, Bytes response)
    {
        transport_.Exchange(Req(request), Reply(response));
    }
    void Alive()
    {
        X({0xb7}, {0x7f, 0xb7, 0x13});
    }
    void Session(Bytes response = {0x50, 0x03})
    {
        X({0x10, 0x03}, std::move(response));
    }
    void Seed(Bytes response = {0x67, 0x01, 0xde, 0xad, 0xbe, 0xef})
    {
        X({0x27, 0x01}, std::move(response));
    }
    void Key(Bytes response = {0x67, 0x02})
    {
        // Independent Feistel transcription: DEAD BEEF -> DE44 6FC3, reverse
        // Normal2 key table, legacy operation.cpp:1131-1151 at 5dc86672.
        X({0x27, 0x02, 0xde, 0x44, 0x6f, 0xc3}, std::move(response));
    }
    void Access()
    {
        Session();
        Seed();
        Key();
    }
    Bytes PageRequest(std::uint32_t a)
    {
        return bytes::ComposeBe(0x23_b, 0x24_b, a, std::uint16_t{1024});
    }
    Bytes Pages()
    {
        Bytes expected;
        expected.reserve(0x200000);
        for (std::uint32_t a = 0; a < 0x200000; a += 0x400)
        {
            Bytes page{0x63};
            for (unsigned i = 0; i < 0x400; ++i)
            {
                auto b = static_cast<std::uint8_t>((a / 0x400 + i) % 256U);
                page.push_back(b);
                expected.push_back(b);
            }
            X(PageRequest(a), std::move(page));
        }
        return expected;
    }
    void Stop(bool silence = false)
    {
        for (int i = 0; i < (silence ? 6 : 1); ++i)
        {
            transport_.ExpectWrite(Req(Bytes{0x10, 1}));
            if (silence)
            {
                transport_.QueueNoFrame();
            }
            else
            {
                transport_.QueueRead(Reply(Bytes{0x50, 1}));
            }
        }
    }
    void Init(Bytes ecu = {0xea, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55}, Bytes cal = {0x49, 4, 0, 'C', 'A', 'L'})
    {
        X({0xb7}, {0x7f, 0xb7, 0x22});
        X({0xaa}, std::move(ecu));
        X({9, 2}, {0x49, 2, 0, 'V', 'I', 'N'});
        X({9, 4}, std::move(cal));
        X({9, 6}, {0x49, 6, 0, 0x12, 0x34});
        X({0xa8, 0, 0, 0, 0xd7}, {});
        X({0xa8, 0, 0, 1, 0x3b}, {});
    }
};
TEST_F(Sh72543rExecutor, SetupAndForeignPlanRejectBeforeIo)
{
    auto cfg = executor_.TransportSetup(Plan());
    ASSERT_THAT(cfg, fastecu::testing::IsOk());
    EXPECT_EQ(cfg->request_id, 0x7e0U);
    EXPECT_EQ(cfg->response_id, 0x7e8U);
    EXPECT_EQ(cfg->bitrate, 500000);
    EXPECT_FALSE(cfg->extended_id);
    auto foreign =
        BuildSubaruTcuHitachiM32rCanPlan(FlashOperation::kRead, "sub_tcu_hitachi_m32r_can", "M32R_512KB", std::nullopt);
    ASSERT_THAT(foreign, fastecu::testing::IsOk());
    EXPECT_THAT(executor_.TransportSetup(*foreign), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(executor_.Execute(*foreign, transport_, clock_, cancel_, events_),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(transport_.writes, 0U);
}
TEST_F(Sh72543rExecutor, CompleteReadPinsBytesTimingAndKernelShortcut)
{
    Alive();
    Access();
    auto expected = Pages();
    Stop();
    auto result = Run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->read_bytes, expected);
    EXPECT_FALSE(result->rom_id);
    EXPECT_TRUE(transport_.ScriptConsumed());
    EXPECT_EQ(transport_.writes, 2053U);
    EXPECT_EQ(clock_.Elapsed(), 850ms);
    ASSERT_EQ(transport_.ReadTimeouts().size(), 2053U);
    EXPECT_EQ(transport_.ReadTimeouts().front(), 200ms);
    EXPECT_EQ(transport_.ReadTimeouts().back(), 800ms);
    for (std::size_t i = 1; i + 1 < transport_.ReadTimeouts().size(); ++i)
    {
        EXPECT_EQ(transport_.ReadTimeouts()[i], 2000ms);
    }
    ASSERT_FALSE(events_.phase_progress_calls.empty());
    EXPECT_EQ(events_.phase_progress_calls.back().done, events_.phase_progress_calls.back().total);
}
TEST_F(Sh72543rExecutor, FullInitializationReturnsIdentityAndRecoveryAlias)
{
    Init();
    Access();
    auto expected = Pages();
    Stop();
    auto result = executor_.Execute(Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh72543r_can_recovery"), transport_,
                                    clock_, cancel_, events_);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, "CAL_1122334455_");
    EXPECT_EQ(result->read_bytes, expected);
    EXPECT_TRUE(transport_.ScriptConsumed());
    EXPECT_EQ(clock_.Elapsed(), 1450ms);
}
TEST_F(Sh72543rExecutor, PartialIdentityDoesNotReadPastReply)
{
    Init({0xea}, {0x49, 4, 0, 'C', 'A', 'L'});
    Access();
    auto expected = Pages();
    Stop(true);
    auto result = Run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, "CAL_");
    EXPECT_EQ(result->read_bytes, expected);
    EXPECT_TRUE(transport_.ScriptConsumed());
}
TEST_F(Sh72543rExecutor, ToleratedSessionAndMissingIdentityStillRead)
{
    Init({0xea}, {0x49, 4});
    Session({0x7f, 0x10, 0x22});
    Seed();
    Key();
    auto expected = Pages();
    Stop();
    auto result = Run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->rom_id);
    EXPECT_EQ(result->read_bytes, expected);
}
TEST_F(Sh72543rExecutor, ShortSeedStopsBeforeKey)
{
    for (unsigned count = 0; count < 4; ++count)
    {
        Transport t;
        t.Exchange(Req(Bytes{0xb7}), Reply(Bytes{0x7f, 0xb7, 0x13}));
        t.Exchange(Req(Bytes{0x10, 3}), Reply(Bytes{0x50, 3}));
        Bytes s{0x67, 1};
        s.resize(2 + count, 0xde);
        t.Exchange(Req(Bytes{0x27, 1}), Reply(s));
        EXPECT_THAT(executor_.Execute(Plan(), t, clock_, cancel_, events_),
                    fastecu::testing::IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(t.ScriptConsumed());
        EXPECT_EQ(t.writes, 3U);
    }
}
TEST_F(Sh72543rExecutor, WrongKeyStopsBeforePage)
{
    Alive();
    Session();
    Seed();
    Key({0x67, 3});
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport_.ScriptConsumed());
}
TEST_F(Sh72543rExecutor, RejectsMalformedPagesWithoutPartialSuccess)
{
    for (auto size : {0U, 1U, 0x400U, 0x402U})
    {
        Transport t;
        t.Exchange(Req(Bytes{0xb7}), Reply(Bytes{0x7f, 0xb7, 0x13}));
        t.Exchange(Req(Bytes{0x10, 3}), Reply(Bytes{0x50, 3}));
        t.Exchange(Req(Bytes{0x27, 1}), Reply(Bytes{0x67, 1, 0xde, 0xad, 0xbe, 0xef}));
        t.Exchange(Req(Bytes{0x27, 2, 0xde, 0x44, 0x6f, 0xc3}), Reply(Bytes{0x67, 2}));
        Bytes p(size, 0x63);
        t.Exchange(Req(PageRequest(0)), Reply(p));
        EXPECT_THAT(executor_.Execute(Plan(), t, clock_, cancel_, events_),
                    fastecu::testing::IsErr(ErrorKind::kBadResponse));
        EXPECT_EQ(t.writes, 5U);
        EXPECT_TRUE(t.ScriptConsumed());
    }
}
TEST_F(Sh72543rExecutor, WrongPageServiceIsRejected)
{
    Alive();
    Access();
    X(PageRequest(0), Bytes(0x401, 0xf7));
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport_.ScriptConsumed());
}
TEST_F(Sh72543rExecutor, RequiredPageTimeoutIsFatal)
{
    Alive();
    Access();
    transport_.ExpectWrite(Req(PageRequest(0)));
    transport_.QueueNoFrame();
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport_.ScriptConsumed());
}
TEST_F(Sh72543rExecutor, CancellationAfterReadStopsNextCommand)
{
    Alive();
    Access();
    transport_.after_read = [&](std::size_t n)
    {
        if (n == 3)
        {
            cancel_.Cancel();
        }
    };
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport_.writes, 4U);
}
TEST_F(Sh72543rExecutor, DisconnectAndFailedWritesAreFatal)
{
    transport_.fail_write = 0;
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    transport_.fail_write.reset();
    transport_.fail_read = 0;
    Alive();
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
}
TEST_F(Sh72543rExecutor, CancelledBeforeStartDoesNoIo)
{
    cancel_.Cancel();
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport_.writes, 0U);
}
TEST_F(Sh72543rExecutor, CancellationDuringPagesDoesNotPublishPartialImage)
{
    Alive();
    Access();
    Pages();
    Stop();
    transport_.after_read = [&](std::size_t n)
    {
        if (n == 100)
        {
            cancel_.Cancel();
        }
    };
    EXPECT_THAT(Run(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport_.writes, 101U);
    EXPECT_LT(events_.phase_progress_calls.back().done, events_.phase_progress_calls.back().total);
}
TEST_F(Sh72543rExecutor, EcuOnlyIdentityAndExplicitOptionalTimeoutArePreserved)
{
    Init({0xea, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55}, {0x49, 4});
    Access();
    Pages();
    Stop();
    auto result = Run();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, "1122334455_");
}
TEST_F(Sh72543rExecutor, ExplicitTimeoutDuringOptionalSessionIsTolerated)
{
    Alive();
    transport_.ExpectWrite(Req(Bytes{0x10, 3}));
    transport_.QueueError(ErrorKind::kTimeout);
    Seed();
    Key();
    Pages();
    Stop();
    EXPECT_THAT(Run(), fastecu::testing::IsOk());
}
TEST_F(Sh72543rExecutor, ForgedDryRunAndWireChangesAreRejectedBeforeIo)
{
    for (int mutation = 0; mutation < 2; ++mutation)
    {
        FlashPlanFields f{.operation = FlashOperation::kTestWrite,
                          .family = FlashFamily::kSubaruHitachiSh72543rCan,
                          .transport = TransportKind::kCanIso15765,
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
            f.operation = FlashOperation::kWrite;
            std::get<SubaruHitachiSh72543rCanPlan>(f.family_plan).request_id = 0x7e1;
        }
        auto built = ValidateAndBuild(std::move(f));
        ASSERT_THAT(built, fastecu::testing::IsOk());
        auto error = mutation ? ErrorKind::kInvalidConfig : ErrorKind::kUnsupported;
        EXPECT_THAT(executor_.TransportSetup(*built), fastecu::testing::IsErr(error));
        EXPECT_THAT(executor_.Execute(*built, transport_, clock_, cancel_, events_), fastecu::testing::IsErr(error));
        EXPECT_EQ(transport_.writes, 0U);
    }
}

// Independent test oracle, two 16-bit halves rather than production's packed
// word transform. Literal vectors below were also evaluated in Python from the
// legacy key schedule. Never call the production cipher to form expected frames.
std::uint32_t ReferenceCipher(std::uint32_t word)
{
    constexpr std::array<unsigned, 32> kBox{5,  6, 7, 1, 9,  12, 13, 8, 10, 13, 2, 11, 15, 4,  0,  3,
                                            11, 4, 6, 0, 15, 2,  13, 9, 5,  12, 1, 10, 3,  13, 14, 8};
    std::uint32_t left = word >> 16U;
    std::uint32_t right = word & 0xffffU;
    for (std::uint32_t round : {0xb740U, 0x42daU, 0xa7caU, 0x5fb1U})
    {
        std::uint32_t index = right ^ round;
        index |= index << 16U;
        std::uint32_t f = 0;
        for (unsigned n = 0; n < 4; ++n)
        {
            f |= kBox[(index >> (4U * n)) & 31U] << (4U * n);
        }
        f = ((f >> 3U) | (f << 13U)) & 0xffffU;
        std::uint32_t next = left ^ f;
        left = right;
        right = next;
    }
    return (right << 16U) | left;
}
TEST(Sh72543rCipherOracle, LiteralVectors)
{
    EXPECT_EQ(ReferenceCipher(0), 0x98a49de7U);
    EXPECT_EQ(ReferenceCipher(0x00010203), 0x5aef57efU);
    EXPECT_EQ(ReferenceCipher(0xdeadbeef), 0xfe3eb636U);
    EXPECT_EQ(ReferenceCipher(0xffffffff), 0x941478d7U);
}
class Sh72543rWrite : public Sh72543rExecutor
{
  protected:
    Bytes Image()
    {
        Bytes b;
        b.reserve(0x200000);
        for (std::uint32_t address = 0; address < 0x200000; address += 4)
        {
            bytes::AppendU32Be(b, 0xdeadbeefU ^ address);
        }
        return b;
    }
    Result<FlashExecutionResult> Write()
    {
        auto p = BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kWrite, "sub_ecu_hitachi_sh72543r_can", "SH72543R",
                                                   Image());
        EXPECT_THAT(p, fastecu::testing::IsOk());
        return executor_.Execute(*p, transport_, clock_, cancel_, events_);
    }
    void ScriptSetup()
    {
        Alive();
        X({0x10, 0x43}, {0x7f, 0x10, 0x22});
        Seed();
        Key();
        X({0x10, 0x42}, {0x50, 0x42});
        X({0x34, 4, 0x33, 0, 0x60, 0, 0x1f, 0xa0, 0}, {0x74, 0x20});
    }
    void Erase(Bytes r = {0x71, 1, 2})
    {
        X({0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}, std::move(r));
    }
    void Data(bool silent = false)
    {
        for (std::uint32_t a = 0x6000; a < 0x200000; a += 0x100)
        {
            Bytes payload = bytes::ComposeBe(0xb6_b, bytes::U24(a));
            for (unsigned offset = 0; offset < 0x100; offset += 4)
            {
                bytes::AppendU32Be(payload, ReferenceCipher(0xdeadbeefU ^ (a + offset)));
            }
            ASSERT_EQ(payload.size(), 260U);
            transport_.ExpectWrite(Req(payload));
            // Legacy does not interpret this content, even negative responses.
            if (silent)
            {
                transport_.QueueNoFrame();
            }
            else
            {
                transport_.QueueRead(Reply(Bytes{0x7f, 0xb6, 0x22}));
            }
        }
    }
    void Finish()
    {
        X({0x37}, {0x77});
        X({0x31, 1, 2, 2, 1}, {0x71, 1, 2});
    }
};
TEST_F(Sh72543rWrite, CompleteWriteUsesAbsoluteOffsetsAndImmediateEraseReply)
{
    ScriptSetup();
    Erase();
    Data();
    Finish();
    auto result = Write();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes);
    EXPECT_FALSE(result->rom_id);
    EXPECT_TRUE(transport_.ScriptConsumed());
    EXPECT_EQ(transport_.writes, 8105U);
    EXPECT_EQ(transport_.reads, 8105U);
    EXPECT_EQ(clock_.Elapsed(), 82210ms);
    EXPECT_EQ(transport_.ReadTimeouts()[8103], 800ms);
    EXPECT_EQ(transport_.ReadTimeouts()[8104], 2000ms);
    int previous_phase = 0, previous_done = 0;
    for (const auto& p : events_.phase_progress_calls)
    {
        EXPECT_GE(p.phase_index, previous_phase);
        if (p.phase_index == previous_phase)
        {
            EXPECT_GE(p.done, previous_done);
        }
        EXPECT_LE(p.done, p.total);
        previous_phase = p.phase_index;
        previous_done = p.done;
    }
    EXPECT_EQ(events_.phase_progress_calls.back().done, events_.phase_progress_calls.back().total);
}
TEST_F(Sh72543rWrite, DelayedEraseAndSilentDataRepliesStillRequireFinalVerification)
{
    ScriptSetup();
    Erase({0x7f, 0x31, 0x78});
    transport_.QueueNoFrame();
    transport_.QueueRead(Reply(Bytes{0x71, 1, 2}));
    Data(true);
    Finish();
    EXPECT_THAT(Write(), fastecu::testing::IsOk());
    EXPECT_TRUE(transport_.ScriptConsumed());
    EXPECT_EQ(transport_.reads, 8107U);
    EXPECT_EQ(clock_.Elapsed(), 82710ms);
}
class Sh72543rWriteFault : public Sh72543rWrite, public ::testing::WithParamInterface<int>
{
};
TEST_P(Sh72543rWriteFault, EraseExhaustionNeverSendsData)
{
    ScriptSetup();
    transport_.ExpectWrite(Req(Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}));
    for (int i = 0; i < 21; ++i)
    {
        if (GetParam())
        {
            transport_.QueueRead(Reply(Bytes{0x7f, 0x31, 0x78}));
        }
        else
        {
            transport_.QueueNoFrame();
        }
    }
    EXPECT_THAT(Write(), fastecu::testing::IsErr(GetParam() ? ErrorKind::kBadResponse : ErrorKind::kTimeout));
    EXPECT_EQ(transport_.writes, 7U);
    EXPECT_EQ(transport_.reads, 27U);
    EXPECT_TRUE(transport_.ScriptConsumed());
}
INSTANTIATE_TEST_SUITE_P(SilenceAndWrongReply, Sh72543rWriteFault, ::testing::Values(0, 1));
class Sh72543rWriteCancel : public Sh72543rWrite, public ::testing::WithParamInterface<int>
{
};
TEST_P(Sh72543rWriteCancel, CancelAtEveryProgrammingPhaseStopsSubsequentIo)
{
    ScriptSetup();
    Erase();
    Data();
    Finish();
    transport_.after_read = [&](std::size_t n)
    {
        if (n == static_cast<std::size_t>(GetParam()))
        {
            cancel_.Cancel();
        }
    };
    EXPECT_THAT(Write(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport_.writes, static_cast<std::size_t>(GetParam() + 1));
    EXPECT_LT(events_.phase_progress_calls.back().done, events_.phase_progress_calls.back().total);
}
// After erase, first/middle/last frame, close, and checksum respectively.
INSTANTIATE_TEST_SUITE_P(Boundaries, Sh72543rWriteCancel, ::testing::Values(6, 7, 100, 8102, 8103, 8104));
TEST_F(Sh72543rWrite, CancelDuringErasePollingStopsBeforeData)
{
    ScriptSetup();
    Erase({0x7f, 0x31, 0x78});
    transport_.QueueNoFrame();
    transport_.after_read = [&](std::size_t n)
    {
        if (n == 7)
        {
            cancel_.Cancel();
        }
    };
    EXPECT_THAT(Write(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport_.writes, 7U);
}
TEST_F(Sh72543rWrite, DisconnectDuringEraseStopsBeforeData)
{
    ScriptSetup();
    Erase();
    transport_.fail_read = 6;
    EXPECT_THAT(Write(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport_.writes, 7U);
}
TEST_F(Sh72543rWrite, FailedDataWriteStopsBeforeFinalization)
{
    ScriptSetup();
    Erase();
    Data();
    Finish();
    transport_.fail_write = 7;
    EXPECT_THAT(Write(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport_.writes, 8U);
}
TEST_F(Sh72543rWrite, DisconnectedDataReadStopsBeforeFinalization)
{
    ScriptSetup();
    Erase();
    Data();
    Finish();
    transport_.fail_read = 7;
    EXPECT_THAT(Write(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport_.writes, 8U);
}
class Sh72543rFinalizeFault : public Sh72543rWrite, public ::testing::WithParamInterface<int>
{
};
TEST_P(Sh72543rFinalizeFault, BoundedFinalizationRetriesClassifyFailure)
{
    ScriptSetup();
    Erase();
    Data();
    bool checksum = GetParam() >= 2, response = GetParam() % 2;
    if (checksum)
    {
        X({0x37}, {0x77});
    }
    for (int i = 0; i < 20; ++i)
    {
        transport_.ExpectWrite(Req(checksum ? Bytes{0x31, 1, 2, 2, 1} : Bytes{0x37}));
        if (response)
        {
            transport_.QueueRead(Reply(Bytes{0x7f, 0x31, 0x78}));
        }
        else
        {
            transport_.QueueNoFrame();
        }
    }
    EXPECT_THAT(Write(), fastecu::testing::IsErr(response ? ErrorKind::kBadResponse : ErrorKind::kTimeout));
    EXPECT_TRUE(transport_.ScriptConsumed());
    EXPECT_EQ(transport_.writes, static_cast<std::size_t>(8103 + 20 + (checksum ? 1 : 0)));
}
INSTANTIATE_TEST_SUITE_P(CloseAndChecksum, Sh72543rFinalizeFault, ::testing::Values(0, 1, 2, 3));
TEST_F(Sh72543rWrite, LastRetrySucceedsWithoutEarlyCompletion)
{
    ScriptSetup();
    Erase();
    Data();
    for (int i = 0; i < 19; ++i)
    {
        X({0x37}, {0x7f, 0x37, 0x78});
    }
    X({0x37}, {0x77});
    for (int i = 0; i < 19; ++i)
    {
        X({0x31, 1, 2, 2, 1}, {0x7f, 0x31, 0x78});
    }
    X({0x31, 1, 2, 2, 1}, {0x71, 1, 2});
    EXPECT_THAT(Write(), fastecu::testing::IsOk());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(Sh72543rWrite, ReusedExecutorDoesNotLeakReadMetadataIntoWrite)
{
    Init();
    Access();
    Pages();
    Stop();
    auto read = Run();
    ASSERT_THAT(read, fastecu::testing::IsOk());
    EXPECT_EQ(read->rom_id, "CAL_1122334455_");
    ScriptSetup();
    Erase();
    Data();
    Finish();
    auto result = Write();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->read_bytes);
    EXPECT_FALSE(result->rom_id);
    EXPECT_TRUE(transport_.ScriptConsumed());
}
} // namespace
