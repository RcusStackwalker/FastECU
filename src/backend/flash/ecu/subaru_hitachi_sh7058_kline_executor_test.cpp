#include "src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/testing/faulting_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/faulting_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

using ::testing::_;
using ::testing::IsEmpty;
using ::testing::Optional;

namespace fastecu::flash
{
TEST(SubaruHitachiSh7058KlineExecutor, PrecancelledRunIsRefusedInAdmission)
{
    const auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                   std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancel{true};
    RecordingEventSink events;
    EXPECT_THAT(SubaruHitachiSh7058KlineExecutor{}.Execute(*plan, transport, clock, cancel, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_THAT(transport.header_mode_calls, IsEmpty());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruHitachiSh7058KlineExecutor, WriteOperationIsRefusedBeforeThePrecancelCheck)
{
    const auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kWrite, "sub_ecu_hitachi_sh7058_can",
                                                   "SH7058_1block", bytes::Bytes(0x100000));
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancel{true};
    RecordingEventSink events;
    EXPECT_THAT(
        SubaruHitachiSh7058KlineExecutor{}.Execute(*plan, transport, clock, cancel, events),
        fastecu::testing::IsErrWith(ErrorKind::kUnsupported, ::testing::Eq("SH7058 K-Line supports read only")));
}

TEST(SubaruHitachiSh7058KlineExecutor, RejectsMissingInitializationBeforeReadPages)
{
    auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                             std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruHitachiSh7058KlineExecutor executor;
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueNoFrame();
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.Execute(*plan, transport, clock, cancel, events);
    // A silent 38400 init is not rejected: the executor falls back to the 4800
    // identity exchange, whose write runs past this one-exchange script.
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInternal,
                                                    ::testing::HasSubstr("ran past the end of the script")));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruHitachiSh7058KlineExecutor, ReadsEveryPhysicalPageBeforeReturningRom)
{
    for (const bool already_active : {true, false})
    {
        SCOPED_TRACE(::testing::PrintToString(already_active));
        auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport;
        const auto frame = [](bytes::ByteView payload, bool response)
        {
            return response ? ssm_protocol::AddHeader(payload, 0x10, 0xf0)
                            : ssm_protocol::AddHeader(payload, 0xf0, 0x10);
        };
        if (already_active)
        {
            transport.Exchange(frame(bytes::Bytes{0xbf}, false), frame(bytes::Bytes{0xff}, true));
        }
        else
        {
            transport.ExpectWrite(frame(bytes::Bytes{0xbf}, false));
            transport.QueueNoFrame();
            transport.Exchange(frame(bytes::Bytes{0xbf}, false),
                               frame(bytes::Bytes{0xff, 0, 0, 0, 0, 1, 2, 3, 4}, true));
            transport.Exchange(frame(bytes::Bytes{0xb8, 0, 0, 0, 0x75}, false), frame(bytes::Bytes{0xf8}, true));
            transport.Exchange(frame(bytes::Bytes{0xbf}, false), frame(bytes::Bytes{0xff}, true));
        }
        for (std::uint32_t offset = 0; offset < 0x100000; offset += 0x80)
        {
            const std::uint32_t address = 0x100000 + offset;
            const auto request =
                bytes::ComposeBe(bytes::Byte{0xa0}, bytes::Byte{0}, bytes::U24{address}, bytes::Byte{0x7f});
            bytes::Bytes response(129, static_cast<bytes::Byte>(offset / 0x80));
            response[0] = 0xe0;
            transport.Exchange(frame(request, false), frame(response, true));
        }
        SubaruHitachiSh7058KlineExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancel;
        RecordingEventSink events;
        auto result = executor.Execute(*plan, transport, clock, cancel, events);
        ASSERT_THAT(result, fastecu::testing::IsOk());
        ASSERT_THAT(result->read_bytes, Optional(_));
        EXPECT_EQ(result->read_bytes->size(), 0x100000U);
        if (!already_active)
        {
            EXPECT_EQ(result->rom_id, std::optional<std::string>("0001020304_"));
        }
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, RejectsShortWrongServiceAndBadChecksumPages)
{
    for (int fault = 0; fault < 3; ++fault)
    {
        SCOPED_TRACE(::testing::PrintToString(fault));
        auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport;
        transport.Exchange(ssm_protocol::AddHeader(bytes::Bytes{0xbf}, 0xf0, 0x10),
                           ssm_protocol::AddHeader(bytes::Bytes{0xff}, 0x10, 0xf0));
        bytes::Bytes payload(fault == 0 ? 128 : 129, 0);
        payload[0] = fault == 1 ? 0xe1 : 0xe0;
        auto response = ssm_protocol::AddHeader(payload, 0x10, 0xf0);
        if (fault == 2)
        {
            response.back() ^= 0x01U;
        }
        transport.Exchange(ssm_protocol::AddHeader(bytes::Bytes{0xa0, 0, 0x10, 0, 0, 0x7f}, 0xf0, 0x10), response);
        SubaruHitachiSh7058KlineExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancel;
        RecordingEventSink events;
        auto result = executor.Execute(*plan, transport, clock, cancel, events);
        EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, CancellationAndBaudFailureStopBeforeAnyRequest)
{
    auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                             std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruHitachiSh7058KlineExecutor executor;
    for (const bool cancel_first : {true, false})
    {
        SCOPED_TRACE(::testing::PrintToString(cancel_first));
        ScriptedKlineFlashTransport transport;
        if (!cancel_first)
        {
            transport.set_baud_result = Fail(ErrorKind::kDisconnected, "baud failed");
        }
        FakeClock clock;
        FakeCancellationToken cancel(cancel_first);
        RecordingEventSink events;
        auto result = executor.Execute(*plan, transport, clock, cancel, events);
        EXPECT_THAT(result, fastecu::testing::IsErr(cancel_first ? ErrorKind::kCancelled : ErrorKind::kDisconnected));
        EXPECT_EQ(transport.WritesConsumed(), 0U);
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, RejectsMalformedIdentityBeforeBaudSwitch)
{
    auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                             std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport;
    const auto request = ssm_protocol::AddHeader(bytes::Bytes{0xbf}, 0xf0, 0x10);
    transport.ExpectWrite(request);
    transport.QueueNoFrame();
    transport.Exchange(request, ssm_protocol::AddHeader(bytes::Bytes{0xfe}, 0x10, 0xf0));
    SubaruHitachiSh7058KlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.Execute(*plan, transport, clock, cancel, events);
    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

namespace
{
bytes::Bytes Request(bytes::ByteView payload)
{
    return ssm_protocol::AddHeader(payload, 0xf0, 0x10);
}

bytes::Bytes Reply(bytes::ByteView payload)
{
    return ssm_protocol::AddHeader(payload, 0x10, 0xf0);
}

// A silent 38400 init, the 4800 identity, the baud switch and the resumed
// init: replies[i] answers the i-th exchange.
std::vector<bytes::Bytes> FallbackReplies()
{
    return {bytes::Bytes{}, Reply(bytes::Bytes{0xff, 0, 0, 0, 0, 1, 2, 3, 4}), Reply(bytes::Bytes{0xf8}),
            Reply(bytes::Bytes{0xff})};
}

// The fallback, then the first ROM page; an empty reply is a silent read.
void ScriptFallbackAndFirstPage(ScriptedKlineFlashTransport& transport,
                                const std::vector<bytes::Bytes>& replies = FallbackReplies())
{
    const std::array<bytes::Bytes, 4> requests{Request(bytes::Bytes{0xbf}), Request(bytes::Bytes{0xbf}),
                                               Request(bytes::Bytes{0xb8, 0, 0, 0, 0x75}), Request(bytes::Bytes{0xbf})};
    for (std::size_t i = 0; i < replies.size(); ++i)
    {
        transport.ExpectWrite(requests[i]);
        if (replies[i].empty())
        {
            transport.QueueNoFrame();
        }
        else
        {
            transport.QueueRead(replies[i]);
        }
    }
    if (replies.size() == requests.size())
    {
        bytes::Bytes page(129, 0);
        page[0] = 0xe0;
        transport.Exchange(Request(bytes::Bytes{0xa0, 0, 0x10, 0, 0, 0x7f}), Reply(page));
    }
}

Result<FlashExecutionResult> RunRead(ScriptedKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancel)
{
    auto plan = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                             std::nullopt);
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    RecordingEventSink events;
    return SubaruHitachiSh7058KlineExecutor{}.Execute(*plan, transport, clock, cancel, events);
}

class ShortFirstWriteTransport final : public ScriptedKlineFlashTransport
{
  public:
    Result<std::size_t> Write(bytes::ByteView data) override
    {
        auto written = ScriptedKlineFlashTransport::Write(data);
        if (written.has_value() && WritesConsumed() == 1)
        {
            return *written - 1;
        }
        return written;
    }
};
} // namespace

TEST(SubaruHitachiSh7058KlineExecutor, EveryTransportFaultOnTheFallbackPathPropagates)
{
    for (const auto& [op, count] : std::to_array<std::pair<KlineFaultOp, std::size_t>>({
             {KlineFaultOp::kHeaderMode, 1},
             {KlineFaultOp::kSetBaud, 3},
             {KlineFaultOp::kWrite, 5},
             {KlineFaultOp::kRead, 5},
         }))
    {
        SCOPED_TRACE(::testing::PrintToString(static_cast<int>(op)));
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::PrintToString(std::pair{static_cast<int>(op), k}));
            FaultingKlineFlashTransport transport;
            transport.op = op;
            transport.fail_at = k;
            ScriptFallbackAndFirstPage(transport);
            FakeClock clock;
            FakeCancellationToken cancel;
            EXPECT_THAT(RunRead(transport, clock, cancel),
                        fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
            EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>(transport.WritesConsumed()));
        }
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, EverySleepFailureOnTheFallbackPathPropagates)
{
    for (std::size_t k = 1; k <= 5; ++k)
    {
        SCOPED_TRACE(::testing::PrintToString(k));
        ScriptedKlineFlashTransport transport;
        ScriptFallbackAndFirstPage(transport);
        FaultingClock clock;
        clock.fail_at = k;
        FakeCancellationToken cancel;
        EXPECT_THAT(RunRead(transport, clock, cancel),
                    fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("sleep failed")));
        EXPECT_EQ(transport.WritesConsumed(), k);
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, CancellingOnTheFallbackPathWritesNothingMore)
{
    for (std::size_t k = 2;; ++k)
    {
        SCOPED_TRACE(::testing::PrintToString(k));
        ASSERT_LT(k, 64U);
        ScriptedKlineFlashTransport transport;
        ScriptFallbackAndFirstPage(transport);
        FakeClock clock;
        FakeCancellationToken cancel;
        std::size_t checks = 0;
        std::optional<std::size_t> writes_at_cancel;
        cancel.SetPredicate(
            [&]
            {
                if (++checks == k && !transport.ScriptConsumed())
                {
                    writes_at_cancel = transport.WritesConsumed();
                }
                return writes_at_cancel.has_value();
            });
        const auto result = RunRead(transport, clock, cancel);
        if (!writes_at_cancel.has_value())
        {
            break;
        }
        EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(transport.WritesConsumed(), *writes_at_cancel);
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, AShortWriteIsADisconnect)
{
    ShortFirstWriteTransport transport;
    transport.ExpectWrite(Request(bytes::Bytes{0xbf}));
    FakeClock clock;
    FakeCancellationToken cancel;
    EXPECT_THAT(RunRead(transport, clock, cancel),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("short SH7058 K-Line write")));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruHitachiSh7058KlineExecutor, ARejectedBaudSwitchOrLostResumeFails)
{
    for (const auto& [k, detail] : std::to_array<std::pair<std::size_t, std::string_view>>({
             {2, "SH7058 baud switch rejected"},
             {3, "SH7058 connection lost after baud switch"},
         }))
    {
        SCOPED_TRACE(detail);
        std::vector<bytes::Bytes> replies = FallbackReplies();
        replies[k] = Reply(bytes::Bytes{0xf7});
        replies.resize(k + 1);
        ScriptedKlineFlashTransport transport;
        ScriptFallbackAndFirstPage(transport, replies);
        FakeClock clock;
        FakeCancellationToken cancel;
        EXPECT_THAT(RunRead(transport, clock, cancel),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::Eq(detail)));
        EXPECT_EQ(transport.WritesConsumed(), k + 1);
    }
}
} // namespace fastecu::flash
