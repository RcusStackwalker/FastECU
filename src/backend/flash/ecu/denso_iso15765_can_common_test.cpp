#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::flash
{
namespace
{

TEST(DensoIso15765CanCommon, SetupPduEncodesDownloadBigEndian)
{
    const auto pdu = DensoIso15765DownloadSetupPdu(MemoryRegion{0x01234567, 0x89ABCDEF});
    static_assert(std::same_as<decltype(pdu), const std::array<bytes::Byte, 11>>);
    EXPECT_THAT(pdu,
                test_bytes::BytesEq((bytes::Bytes{0x34, 0x04, 0x44, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF})));
}

TEST(DensoIso15765CanCommon, SetupPduEncodesUploadBigEndian)
{
    const auto pdu = DensoIso15765UploadSetupPdu(MemoryRegion{0x01234567, 0x89ABCDEF});
    static_assert(std::same_as<decltype(pdu), const std::array<bytes::Byte, 11>>);
    EXPECT_THAT(pdu,
                test_bytes::BytesEq((bytes::Bytes{0x35, 0x04, 0x44, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF})));
}

TEST(DensoIso15765CanCommon, SetupPduEncodesZeroFields)
{
    EXPECT_THAT(DensoIso15765DownloadSetupPdu(MemoryRegion{0, 0}),
                test_bytes::BytesEq((bytes::Bytes{0x34, 0x04, 0x44, 0, 0, 0, 0, 0, 0, 0, 0})));
}

TEST(DensoIso15765CanCommon, SetupPduEncodesMaximumFields)
{
    EXPECT_THAT(DensoIso15765UploadSetupPdu(MemoryRegion{0xFFFFFFFF, 0xFFFFFFFF}),
                test_bytes::BytesEq((bytes::Bytes{0x35, 0x04, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})));
}

TEST(DensoIso15765CanCommon, UploadSetupPduEncodesSmallFields)
{
    EXPECT_THAT(DensoIso15765UploadSetupPdu(MemoryRegion{1, 0x100}),
                test_bytes::BytesEq((bytes::Bytes{0x35, 0x04, 0x44, 0, 0, 0, 1, 0, 0, 1, 0})));
}

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::ManualCancellationToken;
using fastecu::MockClock;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::Status;
using fastecu::flash::testing::Request;
using fastecu::flash::testing::RequestTo;
using fastecu::flash::testing::Response;

// Cancels the operator's token once a given log line has been emitted, so a
// test can stop the flow at an exact point between two exchanges.
class CancellingEventSink : public RecordingEventSink
{
  public:
    CancellingEventSink(ManualCancellationToken& token, std::string_view trigger) : token_(token), trigger_(trigger)
    {
    }
    void Log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::Log(level, message);
        if (message == trigger_)
        {
            token_.Cancel();
        }
    }

  private:
    ManualCancellationToken& token_;
    std::string_view trigger_;
};

// Best-effort operations swallow rejected writes, so count every attempt,
// including writes rejected by the scripted transport's wire expectations.
class WriteCountingCanTransport final : public ScriptedCanFlashTransport
{
  public:
    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        ++write_attempts;
        return ScriptedCanFlashTransport::Write(data, cancellation);
    }

    std::size_t write_attempts = 0;
};

// The real UDS stack over the scripted transport, on the 0x7E0/0x7E8 pair.
struct CommonFixture
{
    WriteCountingCanTransport transport;
    CanFlashUdsChannel channel{transport, 0x7E0, 0x7E8};
    RecordingClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    uds::UdsClient client{channel, clock, events};
    CanExecutorContext ctx{cancellation, events, clock, client, channel};
};

TEST(DensoIso15765CanCommon, JumpWaitChecksCurrentReplyBeforeEachSleep)
{
    CommonFixture immediate;
    EXPECT_THAT(DensoIso15765WaitForJumpAck(immediate.ctx, 0x42, bytes::Bytes{0x50, 0x42}, 50, 200ms),
                fastecu::testing::IsOk());
    EXPECT_THAT(immediate.clock.sleep_calls, ::testing::IsEmpty());
    EXPECT_THAT(immediate.transport.ReadTimeouts(), ::testing::IsEmpty());
    EXPECT_THAT(immediate.events.logs,
                ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Kernel jump acknowledged")));

    // Absent, short, wrong-service and wrong-session replies must all wait.
    for (const auto& reply : std::array<std::optional<bytes::Bytes>, 5>{
             std::nullopt, bytes::Bytes{}, bytes::Bytes{0x50}, bytes::Bytes{0x51, 0x62}, bytes::Bytes{0x50, 0x42}})
    {
        SCOPED_TRACE(::testing::PrintToString(reply));
        CommonFixture late;
        late.transport.QueueRead(Response({0x50, 0x62, 0xAA}));
        EXPECT_THAT(DensoIso15765WaitForJumpAck(late.ctx, 0x62, reply, 10, 200ms), fastecu::testing::IsOk());
        EXPECT_TRUE(late.transport.ScriptConsumed());
        EXPECT_THAT(late.clock.sleep_calls, ::testing::ElementsAre(100ms));
        EXPECT_THAT(late.transport.ReadTimeouts(), ::testing::ElementsAre(200ms));
        EXPECT_THAT(late.events.logs,
                    ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Kernel jump acknowledged")));
    }
}

TEST(DensoIso15765CanCommon, JumpWaitFinalReceiveIsNotChecked)
{
    for (const int attempts : {50, 10})
    {
        SCOPED_TRACE(attempts);
        CommonFixture f;
        const bytes::Byte session = attempts == 50 ? 0x42 : 0x62;
        for (int attempt = 0; attempt < attempts - 1; ++attempt)
        {
            f.transport.QueueNoFrame();
        }
        f.transport.QueueRead(attempts == 50 ? Response({0x50, 0x42}) : Response({0x50, 0x62}));
        ::testing::NiceMock<MockClock> clock;
        int sleeps = 0;
        EXPECT_CALL(clock, Sleep(100ms, ::testing::_))
            .Times(attempts)
            .WillRepeatedly(
                [&](std::chrono::milliseconds duration, const ICancellationToken& cancellation)
                {
                    EXPECT_EQ(f.transport.ReadTimeouts().size(), static_cast<std::size_t>(sleeps));
                    ++sleeps;
                    return clock.Fake().Sleep(duration, cancellation);
                });
        CanExecutorContext ctx{f.cancellation, f.events, clock, f.client, f.channel};
        EXPECT_THAT(DensoIso15765WaitForJumpAck(ctx, session, std::nullopt, attempts, 200ms), fastecu::testing::IsOk());
        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_EQ(f.transport.ReadTimeouts().size(), static_cast<std::size_t>(attempts));
        EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::Each(200ms));
        EXPECT_EQ(sleeps, attempts);
        EXPECT_THAT(f.events.logs,
                    ::testing::ElementsAre(::testing::Pair(
                        LogLevel::kError, "Kernel jump was not acknowledged; continuing, as legacy does")));
    }
}

TEST(DensoIso15765CanCommon, JumpWaitPropagatesSleepAndReceiveErrors)
{
    for (const auto kind :
         {ErrorKind::kInvalidConfig, ErrorKind::kTimeout, ErrorKind::kDisconnected, ErrorKind::kBadResponse,
          ErrorKind::kCancelled, ErrorKind::kUnsupported, ErrorKind::kInternal})
    {
        SCOPED_TRACE(fastecu::ToString(kind));
        CommonFixture f;
        ::testing::NiceMock<MockClock> clock;
        EXPECT_CALL(clock, Sleep(100ms, ::testing::_)).WillOnce(::testing::Return(fastecu::Fail(kind, "sleep failed")));
        CanExecutorContext ctx{f.cancellation, f.events, clock, f.client, f.channel};
        const auto slept = DensoIso15765WaitForJumpAck(ctx, 0x42, std::nullopt, 50, 200ms);
        ASSERT_THAT(slept, fastecu::testing::IsErr(kind));
        EXPECT_THAT(slept, fastecu::testing::IsErrWith(kind, ::testing::Eq("sleep failed")));
        EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::IsEmpty());

        f.transport.QueueError(kind, "receive failed");
        const auto received = DensoIso15765WaitForJumpAck(f.ctx, 0x62, std::nullopt, 10, 200ms);
        ASSERT_THAT(received, fastecu::testing::IsErr(kind));
        EXPECT_THAT(received, fastecu::testing::IsErrWith(kind, ::testing::Eq("receive failed")));
        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(100ms));
        EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms));
        EXPECT_THAT(f.events.logs, ::testing::IsEmpty());
    }
}

TEST(DensoIso15765CanCommon, JumpWaitCancelledSleepDoesNotReceive)
{
    CommonFixture f;
    f.cancellation.Cancel();
    EXPECT_THAT(DensoIso15765WaitForJumpAck(f.ctx, 0x42, std::nullopt, 50, 200ms),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::IsEmpty());
    EXPECT_THAT(f.events.logs, ::testing::IsEmpty());
}

// Even a matching first reply is discarded: only the re-read after 50 ms counts.
TEST(DensoIso15765CanCommon, JumpToKernelDiscardsMatchingFirstReplyOnlyWhenAsked)
{
    for (const bool discard_first_reply : {false, true})
    {
        SCOPED_TRACE(discard_first_reply);
        CommonFixture f;
        f.transport.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42}));
        if (discard_first_reply)
        {
            f.transport.QueueRead(Response({0x50, 0x42}));
        }

        EXPECT_THAT(DensoIso15765JumpToKernel(f.ctx, 0x42, 50, 200ms, 200ms, discard_first_reply),
                    fastecu::testing::IsOk());

        EXPECT_TRUE(f.transport.ScriptConsumed());
        if (discard_first_reply)
        {
            EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms, 200ms));
            EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(50ms));
        }
        else
        {
            EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms));
            EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
        }
        EXPECT_THAT(f.events.logs,
                    ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Jump to onboad kernel"),
                                           ::testing::Pair(LogLevel::kInfo, "Kernel jump acknowledged")));
    }
}

// The SH72543 diesel bench shape: first read at 2000 ms, continuation at 200 ms.
TEST(DensoIso15765CanCommon, JumpToKernelReadsFirstAndLoopRepliesAtTheirOwnTimeouts)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x10, 0x42}));
    f.transport.QueueNoFrame();
    f.transport.QueueRead(Response({0x50, 0x42}));

    EXPECT_THAT(DensoIso15765JumpToKernel(f.ctx, 0x42, 50, 2000ms, 200ms, /*discard_first_reply=*/false),
                fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 200ms));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(100ms));
}

TEST(DensoIso15765CanCommon, JumpToKernelPropagatesFirstAndDiscardReadFailures)
{
    for (const bool fail_discard_read : {false, true})
    {
        SCOPED_TRACE(fail_discard_read);
        CommonFixture f;
        f.transport.Exchange(Request({0x10, 0x42}));
        if (fail_discard_read)
        {
            f.transport.QueueNoFrame();
        }
        f.transport.QueueError(ErrorKind::kDisconnected, "jump read failed");

        EXPECT_THAT(DensoIso15765JumpToKernel(f.ctx, 0x42, 50, 200ms, 200ms, /*discard_first_reply=*/true),
                    fastecu::testing::IsErr(ErrorKind::kDisconnected));

        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_THAT(f.events.logs, ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Jump to onboad kernel")));
    }
}

// Changing the retry bound, error tolerance, or timeout breaks this literal transcript.
TEST(DensoIso15765CanCommon, TransferExitRetriesSixTimesAndSwallowsReturnedErrors)
{
    for (const bool acknowledged : {false, true})
    {
        SCOPED_TRACE(acknowledged);
        CommonFixture f;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            f.transport.Exchange(Request({0x37}));
            if (attempt == 5 && acknowledged)
            {
                f.transport.QueueRead(Response({0x77}));
            }
            else if (attempt == 1)
            {
                f.transport.QueueRead(Response({0x7F, 0x37, 0x31}));
            }
            else if (attempt == 2 || attempt == 3)
            {
                f.transport.QueueError(attempt == 2 ? ErrorKind::kDisconnected : ErrorKind::kCancelled,
                                       "close ignored");
            }
            else
            {
                f.transport.QueueNoFrame();
            }
        }

        DensoIso15765BestEffortTransferExit(f.ctx);

        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_EQ(f.transport.write_attempts, 6U);
        EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms, 200ms, 200ms, 200ms, 200ms, 200ms));
        EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
        if (acknowledged)
        {
            EXPECT_THAT(f.events.logs,
                        ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Closing out Flashing of this block"),
                                               ::testing::Pair(LogLevel::kInfo, "Closed succesfully: 77 ")));
        }
        else
        {
            EXPECT_THAT(f.events.logs,
                        ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Closing out Flashing of this block")));
        }
    }
}

TEST(DensoIso15765CanCommon, TransferExitPendingRereadsWithinOneRequestAndStopsOnSuccess)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x37}), Response({0x7F, 0x37, 0x78}));
    f.transport.QueueRead(Response({0x77, 0x12}));

    DensoIso15765BestEffortTransferExit(f.ctx);

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.write_attempts, 1U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms, 3000ms));
    EXPECT_THAT(
        f.events.logs,
        ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Closing out Flashing of this block"),
                               ::testing::Pair(LogLevel::kDebug, "ECU reported responsePending for SID 0x37; waiting"),
                               ::testing::Pair(LogLevel::kInfo, "Closed succesfully: 77 12 ")));
}

TEST(DensoIso15765CanCommon, TransferExitWithCancelledTokenDoesNoLaterIo)
{
    CommonFixture f;
    CancellingEventSink events(f.cancellation, "Closing out Flashing of this block");
    CanExecutorContext ctx{f.cancellation, events, f.clock, f.client, f.channel};

    DensoIso15765BestEffortTransferExit(ctx);

    EXPECT_TRUE(f.cancellation.Cancelled());
    EXPECT_EQ(f.transport.write_attempts, 0U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::IsEmpty());
    EXPECT_THAT(events.logs,
                ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Closing out Flashing of this block")));
}

// Distinct read and pending timeouts show the family's policy reaches both reads.
TEST(DensoIso15765CanCommon, CloseAndVerifyChecksumSettlesThenSendsLiteralRoutine)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x37}), Response({0x77}));
    f.transport.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}), Response({0x7F, 0x31, 0x78}));
    f.transport.QueueRead(Response({0x71, 0x01, 0x02}));

    EXPECT_THAT(DensoIso15765CloseAndVerifyChecksum(f.ctx, {.read_timeout = 500ms, .pending_timeout = 3000ms}),
                fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms, 500ms, 3000ms));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(100ms));
    EXPECT_THAT(f.events.logs, ::testing::Contains(::testing::Pair(LogLevel::kInfo, "Verifying checksum")));
    EXPECT_THAT(f.events.logs.back(), ::testing::Pair(LogLevel::kInfo, "Checksum verified"));
}

TEST(DensoIso15765CanCommon, CloseAndVerifyChecksumRejectionReturnsErrorWithoutVerifiedLog)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x37}), Response({0x77}));
    f.transport.Exchange(Request({0x31, 0x01, 0x02, 0x02, 0x01}), Response({0x71, 0x01, 0x03}));

    EXPECT_THAT(DensoIso15765CloseAndVerifyChecksum(f.ctx, {.read_timeout = 2000ms}),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.events.logs,
                ::testing::Not(::testing::Contains(::testing::Pair(LogLevel::kInfo, "Checksum verified"))));
}

TEST(DensoIso15765CanCommon, CloseAndVerifyChecksumCancelledSettleSendsNoChecksum)
{
    CommonFixture f;
    CancellingEventSink events(f.cancellation, "Closed succesfully: 77 ");
    CanExecutorContext ctx{f.cancellation, events, f.clock, f.client, f.channel};
    f.transport.Exchange(Request({0x37}), Response({0x77}));

    EXPECT_THAT(DensoIso15765CloseAndVerifyChecksum(ctx, {.read_timeout = 500ms}),
                fastecu::testing::IsErr(ErrorKind::kCancelled));

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.write_attempts, 1U);
    EXPECT_THAT(events.logs,
                ::testing::Not(::testing::Contains(::testing::Pair(LogLevel::kInfo, "Verifying checksum"))));
}

// A literal short chunk pins wire encoding without coupling to image encryption.
TEST(DensoIso15765CanCommon, WriteChunkSendsLiteralB6AbsoluteAddress)
{
    CommonFixture f;
    f.transport.Exchange(Request({0xB6, 0x08, 0xFA, 0xC0, 0x00, 0x11, 0x22}), Response({0xF6}));

    EXPECT_THAT(DensoIso15765WriteChunk(f.ctx, 0x08FAC000, bytes::Bytes{0x11, 0x22}), fastecu::testing::IsOk());
    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(500ms));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
    EXPECT_THAT(f.events.progress_calls, ::testing::IsEmpty());
}

TEST(DensoIso15765CanCommon, WriteChunkPropagatesNegativeTransportAndCancellation)
{
    for (int response_case = 0; response_case < 3; ++response_case)
    {
        SCOPED_TRACE(response_case);
        CommonFixture f;
        f.transport.Exchange(Request({0xB6, 0x08, 0xFA, 0xC0, 0x00, 0x11, 0x22}));
        if (response_case == 0)
        {
            f.transport.QueueRead(Response({0x7F, 0xB6, 0x22}));
        }
        else
        {
            f.transport.QueueError(response_case == 1 ? ErrorKind::kDisconnected : ErrorKind::kCancelled,
                                   "write exchange failed");
        }

        const auto result = DensoIso15765WriteChunk(f.ctx, 0x08FAC000, bytes::Bytes{0x11, 0x22});

        const auto kind = response_case == 0   ? ErrorKind::kBadResponse
                          : response_case == 1 ? ErrorKind::kDisconnected
                                               : ErrorKind::kCancelled;
        ASSERT_THAT(result, fastecu::testing::IsErr(kind));
        if (response_case != 0)
        {
            EXPECT_THAT(result, fastecu::testing::IsErrWith(kind, ::testing::Eq("write exchange failed")));
        }
        if (response_case == 2)
        {
            EXPECT_THAT(f.events.logs, ::testing::Contains(::testing::Pair(
                                           LogLevel::kWarning, ::testing::HasSubstr("the flash write at 0x8fac000"))));
        }
        else
        {
            EXPECT_THAT(f.events.logs, ::testing::Contains(::testing::Pair(
                                           LogLevel::kError, ::testing::StartsWith("Wrong response from ECU: "))));
        }
        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(500ms));
    }
}

// Wrong address encoding, timeout, or accepting a partial crypto word breaks this
// literal page transcript. The helper must leave pacing and progress to callers.
TEST(DensoIso15765CanCommon, ReadPageUsesLiteralB7AddressAndWholeWordDecrypt)
{
    CommonFixture f;
    f.transport.Exchange(Request({0xB7, 0x08, 0xFA, 0xC0, 0x00}),
                         Response({0xF7, 0xE0, 0xD3, 0x85, 0x2B, 0xAA, 0xBB, 0xCC}));

    const auto result = ReadDensoEncryptedPage(f.ctx, 0x08FAC000);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_THAT(*result, test_bytes::BytesEq((bytes::Bytes{0x00, 0x01, 0x02, 0x03})));
    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
    EXPECT_THAT(f.events.progress_calls, ::testing::IsEmpty());
    EXPECT_THAT(f.events.phase_progress_calls, ::testing::IsEmpty());
}

TEST(DensoIso15765CanCommon, ReadPagePropagatesAbsentEmptyTransportAndCancellation)
{
    for (int response_case = 0; response_case < 4; ++response_case)
    {
        SCOPED_TRACE(response_case);
        CommonFixture f;
        f.transport.Exchange(Request({0xB7, 0x08, 0xFA, 0xC0, 0x00}));
        if (response_case == 0)
        {
            f.transport.QueueNoFrame();
        }
        else if (response_case == 1)
        {
            f.transport.QueueRead(bytes::Bytes{});
        }
        else
        {
            f.transport.QueueError(response_case == 2 ? ErrorKind::kDisconnected : ErrorKind::kCancelled,
                                   "page exchange failed");
        }

        const auto result = ReadDensoEncryptedPage(f.ctx, 0x08FAC000);

        const auto kind = response_case == 0   ? ErrorKind::kTimeout
                          : response_case == 1 ? ErrorKind::kBadResponse
                          : response_case == 2 ? ErrorKind::kDisconnected
                                               : ErrorKind::kCancelled;
        ASSERT_THAT(result, fastecu::testing::IsErr(kind));
        if (response_case >= 2)
        {
            EXPECT_THAT(result, fastecu::testing::IsErrWith(kind, ::testing::Eq("page exchange failed")));
        }
        if (response_case == 3)
        {
            EXPECT_THAT(f.events.logs, ::testing::Contains(::testing::Pair(
                                           LogLevel::kWarning, ::testing::HasSubstr("the flash read at 0x8fac000"))));
        }
        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_EQ(f.transport.WritesConsumed(), 1U);
        EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
        EXPECT_THAT(f.events.progress_calls, ::testing::IsEmpty());
        EXPECT_THAT(f.events.phase_progress_calls, ::testing::IsEmpty());
    }
}

// Two short literal pages (four decrypted bytes each) pin addressing, the
// page-to-page order, progress and padding without a full-size region.
void ScriptTwoShortPages(CommonFixture& f)
{
    f.transport.Exchange(Request({0xB7, 0x08, 0xFA, 0xC0, 0x00}), Response({0xF7, 0xE0, 0xD3, 0x85, 0x2B}));
    f.transport.Exchange(Request({0xB7, 0x08, 0xFA, 0xC1, 0x00}), Response({0xF7, 0xE0, 0xD3, 0x85, 0x2B}));
}

std::vector<int> ProgressDone(const RecordingEventSink& events)
{
    return events.phase_progress_calls | std::views::transform([](const auto& call) { return call.done; }) |
           std::ranges::to<std::vector<int>>();
}

TEST(DensoIso15765CanCommon, ReadDumpReadsPagesStopsAndPadsWithPacingOnlyWhenAsked)
{
    for (const bool pace_pages : {false, true})
    {
        SCOPED_TRACE(pace_pages);
        CommonFixture f;
        ScriptTwoShortPages(f);
        f.transport.Exchange(Request({0x37}), Response({0x77}));
        PhaseReporter progress(f.events, "Read ROM", 1, 1, 0x1000);

        const auto image = DensoIso15765ReadDump(f.ctx, MemoryRegion{0x08FAC000, 0x200}, 2, 1, progress, pace_pages);

        ASSERT_THAT(image, fastecu::testing::IsOk());
        EXPECT_THAT(*image, test_bytes::BytesEq(
                                (bytes::Bytes{0xFF, 0xFF, 0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x02, 0x03, 0xFF})));
        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 2000ms, 500ms));
        if (pace_pages)
        {
            EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(1ms, 1ms));
        }
        else
        {
            EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
        }
        EXPECT_THAT(ProgressDone(f.events), ::testing::ElementsAre(0, 0x100, 0x200));
        EXPECT_THAT(f.events.logs,
                    ::testing::ElementsAre(::testing::Pair(LogLevel::kInfo, "Start reading ROM, please wait..."),
                                           ::testing::Pair(LogLevel::kInfo, "ROM read complete"),
                                           ::testing::Pair(LogLevel::kInfo, "Sending stop command"),
                                           ::testing::Pair(LogLevel::kInfo, "Stop request response: 77 ")));
    }
}

// The stop loop's outcome is never checked, so six failures still yield the image.
TEST(DensoIso15765CanCommon, ReadDumpStopRetriesSixTimesAndIgnoresFailures)
{
    CommonFixture f;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        f.transport.Exchange(Request({0x37}));
        if (attempt == 2)
        {
            f.transport.QueueError(ErrorKind::kDisconnected, "stop ignored");
        }
        else
        {
            f.transport.QueueNoFrame();
        }
    }
    PhaseReporter progress(f.events, "Read ROM", 1, 1, 1);

    const auto image = DensoIso15765ReadDump(f.ctx, MemoryRegion{0x8000, 0}, 1, 1, progress, /*pace_pages=*/false);

    ASSERT_THAT(image, fastecu::testing::IsOk());
    EXPECT_THAT(*image, test_bytes::BytesEq((bytes::Bytes{0xFF, 0xFF})));
    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.write_attempts, 6U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::Each(500ms));
}

// The read is already complete when the stop is sent, so a cancellation there is swallowed.
TEST(DensoIso15765CanCommon, ReadDumpCancelledAtStopStillReturnsTheImage)
{
    CommonFixture f;
    CancellingEventSink events(f.cancellation, "Sending stop command");
    CanExecutorContext ctx{f.cancellation, events, f.clock, f.client, f.channel};
    PhaseReporter progress(events, "Read ROM", 1, 1, 1);

    const auto image = DensoIso15765ReadDump(ctx, MemoryRegion{0x8000, 0}, 1, 1, progress, /*pace_pages=*/false);

    ASSERT_THAT(image, fastecu::testing::IsOk());
    EXPECT_THAT(*image, test_bytes::BytesEq((bytes::Bytes{0xFF, 0xFF})));
    EXPECT_TRUE(f.cancellation.Cancelled());
    EXPECT_EQ(f.transport.write_attempts, 0U);
}

TEST(DensoIso15765CanCommon, ReadDumpPageFailureOrCancellationSendsNoStop)
{
    for (const bool cancelled : {false, true})
    {
        SCOPED_TRACE(cancelled);
        CommonFixture f;
        if (cancelled)
        {
            f.cancellation.Cancel();
        }
        else
        {
            f.transport.Exchange(Request({0xB7, 0x08, 0xFA, 0xC0, 0x00}));
            f.transport.QueueError(ErrorKind::kDisconnected, "page lost");
        }
        PhaseReporter progress(f.events, "Read ROM", 1, 1, 0x1000);

        EXPECT_THAT(DensoIso15765ReadDump(f.ctx, MemoryRegion{0x08FAC000, 0x200}, 0, 0, progress,
                                          /*pace_pages=*/true),
                    fastecu::testing::IsErr(cancelled ? ErrorKind::kCancelled : ErrorKind::kDisconnected));

        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_EQ(f.transport.write_attempts, cancelled ? 0U : 1U);
        EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
        EXPECT_THAT(ProgressDone(f.events), ::testing::ElementsAre(0));
        EXPECT_THAT(f.events.logs,
                    ::testing::Not(::testing::Contains(::testing::Pair(LogLevel::kInfo, "Sending stop command"))));
    }
}

// The B6 request for one encrypted chunk at `address`.
bytes::Bytes ChunkRequest(std::uint32_t address, bytes::ByteView encrypted_chunk)
{
    bytes::Bytes out = Request({0xB6});
    bytes::AppendU32Be(out, address);
    out.insert(out.end(), encrypted_chunk.begin(), encrypted_chunk.end());
    return out;
}

// A nonzero image base and a trailing partial chunk: only the one whole chunk
// is sent, taken at the wire address minus the base, never at the block offset.
TEST(DensoIso15765CanCommon, WriteImageSendsWholeChunksIndexedFromImageBase)
{
    CommonFixture f;
    bytes::Bytes image(0x300);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 7);
    }
    f.transport.Exchange(ChunkRequest(0x1100, bytes::ByteView(DensoEncryptRom(image)).subspan(0x100, 0x100)),
                         Response({0xF6}));
    PhaseReporter progress(f.events, "Write", 1, 1, 0x1000);

    EXPECT_THAT(DensoIso15765WriteImage(f.ctx, image, 0x1000, MemoryRegion{0x1100, 0x1FF}, progress),
                fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(ProgressDone(f.events), ::testing::ElementsAre(0, 0x100));
}

// A cancelled or rejected write stops the loop and never reaches the close.
TEST(DensoIso15765CanCommon, WriteImageAndVerifyStopsOnCancellationOrChunkFailureBeforeClosing)
{
    for (const bool cancelled : {false, true})
    {
        SCOPED_TRACE(cancelled);
        CommonFixture f;
        if (cancelled)
        {
            f.cancellation.Cancel();
        }
        else
        {
            f.transport.Exchange(ChunkRequest(0, DensoEncryptRom(bytes::Bytes(0x100))), Response({0x7F, 0xB6, 0x22}));
        }
        PhaseReporter progress(f.events, "Write", 1, 1, 0x1000);

        EXPECT_THAT(DensoIso15765WriteImageAndVerifyChecksum(f.ctx, bytes::Bytes(0x200), 0, MemoryRegion{0, 0x200},
                                                             progress, {.read_timeout = 500ms}),
                    fastecu::testing::IsErr(cancelled ? ErrorKind::kCancelled : ErrorKind::kBadResponse));

        EXPECT_EQ(f.transport.write_attempts, cancelled ? 0U : 1U);
        EXPECT_THAT(ProgressDone(f.events), ::testing::ElementsAre(0));
        EXPECT_THAT(f.events.logs, ::testing::Not(::testing::Contains(
                                       ::testing::Pair(LogLevel::kInfo, "Closing out Flashing of this block"))));
    }
}

TEST(DensoIso15765CanCommon, SecurityAccessReadsLiteralSeedAndKeyAtTwoSeconds)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    f.transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));

    EXPECT_THAT(DensoSecurityAccess(f.ctx), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 2000ms));
}

TEST(DensoIso15765CanCommon, SecurityAccessRereadsPendingReplyWithoutResending)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x7F, 0x27, 0x78}));
    f.transport.QueueRead(Response({0x7F, 0x27, 0x78}));
    f.transport.QueueRead(Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    f.transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));

    EXPECT_THAT(DensoSecurityAccess(f.ctx), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 3000ms, 3000ms, 2000ms));
}

TEST(DensoIso15765CanCommon, SecurityAccessRejectsShortSeedWithoutSendingKey)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33}));

    const Status result = DensoSecurityAccess(f.ctx);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(f.transport.WritesConsumed(), 1U);
}

TEST(DensoIso15765CanCommon, SecurityAccessPropagatesCancellation)
{
    CommonFixture f;
    CancellingEventSink events(f.cancellation, "Seed request ok");
    uds::UdsClient client(f.channel, f.clock, events);
    CanExecutorContext ctx{f.cancellation, events, f.clock, client, f.channel};
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));

    const Status result = DensoSecurityAccess(ctx);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(f.transport.WritesConsumed(), 1U);
}

constexpr MemoryRegion kEraseRegion{0x08FAC000, 0x00173F00};

void ScriptEraseSetup(ScriptedCanFlashTransport& transport)
{
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                       Response({0x74, 0x20, 0x01, 0x05}));
}

void ScriptEraseTrigger(ScriptedCanFlashTransport& transport)
{
    transport.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(DensoIso15765CanCommon, EraseSetupMismatchDoesNotSendTrigger)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                         Response({0x74, 0x20, 0x01, 0x04}));

    const Status result = DensoIso15765Erase(f.ctx, kEraseRegion);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(f.transport.WritesConsumed(), 1U);
}

TEST(DensoIso15765CanCommon, EraseAccepts71_01_02AfterPolling)
{
    CommonFixture f;
    ScriptEraseSetup(f.transport);
    ScriptEraseTrigger(f.transport);
    f.transport.QueueRead(Response({0x71, 0x01, 0x03}));
    f.transport.QueueRead(Response({0x71, 0x01, 0x02, 0x00}));

    EXPECT_THAT(DensoIso15765Erase(f.ctx, kEraseRegion), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.WritesConsumed(), 2U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(500ms, 500ms, 500ms));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(500ms, 500ms));
    EXPECT_THAT(f.events.logs,
                ::testing::ElementsAre(
                    ::testing::Pair(LogLevel::kInfo, "Setting flash start & length"),
                    ::testing::Pair(LogLevel::kInfo, "Erasing ECU ROM"),
                    ::testing::Pair(LogLevel::kInfo, "Flash erased! Starting flash write, do not power off!")));
}

TEST(DensoIso15765CanCommon, ErasePollingStopsAfter20ReceivesAndNeverResendsTrigger)
{
    CommonFixture f;
    ScriptEraseSetup(f.transport);
    ScriptEraseTrigger(f.transport);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        f.transport.QueueRead(Response({0x71, 0x01, 0x03}));
    }

    const Status result = DensoIso15765Erase(f.ctx, kEraseRegion);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.WritesConsumed(), 2U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::Each(500ms));
    EXPECT_EQ(f.transport.ReadTimeouts().size(), 21U);
    EXPECT_THAT(f.clock.sleep_calls, ::testing::Each(500ms));
    EXPECT_EQ(f.clock.sleep_calls.size(), 21U);
    EXPECT_THAT(f.events.logs, ::testing::Contains(::testing::Pair(LogLevel::kError, "Flash area erase failed")));
}

TEST(DensoIso15765CanCommon, EraseCancellationAfterTriggerStopsPolling)
{
    CommonFixture f;
    // Stop the operator's token during the settle sleep that follows the trigger.
    ::testing::NiceMock<MockClock> clock;
    EXPECT_CALL(clock, Sleep).WillOnce(::testing::DoAll([&] { f.cancellation.Cancel(); }, clock.SleepOnFake()));
    uds::UdsClient client(f.channel, clock, f.events);
    CanExecutorContext ctx{f.cancellation, f.events, clock, client, f.channel};
    ScriptEraseSetup(f.transport);
    ScriptEraseTrigger(f.transport);

    const Status result = DensoIso15765Erase(ctx, kEraseRegion);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(f.transport.WritesConsumed(), 2U);
    EXPECT_EQ(f.transport.ReadTimeouts().size(), 1U);
}

// Both N83M families send this literal run, with the in-car 0x7E1 request
// asking for subfunction 0x63. Replies are read and thrown away, so each one
// here carries a deliberately wrong id and content.
TEST(DensoIso15765CanCommon, N83mInCarSequencePreservesBothFamilyTranscripts)
{
    CommonFixture f;
    const std::array<bytes::Bytes, 10> requests{RequestTo(0x7A2, {0x10, 0xC0}), RequestTo(0x7E0, {0x10, 0x63}),
                                                RequestTo(0x7DF, {0x10, 0x03}), RequestTo(0x7E1, {0x10, 0x63}),
                                                RequestTo(0x7B0, {0x10, 0x03}), RequestTo(0x7B0, {0x85, 0x02}),
                                                RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7B0, {0x85, 0x02}),
                                                RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7DF, {0x28, 0x03, 0x01})};
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        f.transport.Exchange(requests[i], RequestTo(0x123 + static_cast<std::uint32_t>(i), {0x7F, 0xEE, 0xEE}));
    }

    EXPECT_THAT(N83mInCarFireAndForget(f.ctx, f.transport), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.WritesConsumed(), 10U);
    EXPECT_EQ(f.transport.ReadTimeouts().size(), 10U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::Each(200ms));
}

// A failed send stops the run before its read; so does a failed read, before the next send.
TEST(DensoIso15765CanCommon, InCarRunStopsOnAFailedSendOrRead)
{
    CommonFixture cancelled;
    cancelled.cancellation.Cancel();
    EXPECT_THAT(N83mInCarFireAndForget(cancelled.ctx, cancelled.transport),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(cancelled.transport.ReadTimeouts(), ::testing::IsEmpty());

    CommonFixture disconnected;
    disconnected.transport.Exchange(RequestTo(0x7A2, {0x10, 0xC0}));
    disconnected.transport.QueueError(ErrorKind::kDisconnected, "adapter gone");
    EXPECT_THAT(N83mInCarFireAndForget(disconnected.ctx, disconnected.transport),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(disconnected.transport.ScriptConsumed());
    EXPECT_EQ(disconnected.transport.write_attempts, 1U);
}

// Session 0x03 on 0x7E1, and only the 0x7E0 reply reads at the primary timeout.
TEST(DensoIso15765CanCommon, ShInCarSequenceSends03To7E1AndReadsThePrimaryReplyAtItsTimeout)
{
    CommonFixture f;
    const std::array<bytes::Bytes, 10> requests{RequestTo(0x7A2, {0x10, 0xC0}), RequestTo(0x7E0, {0x10, 0x63}),
                                                RequestTo(0x7DF, {0x10, 0x03}), RequestTo(0x7E1, {0x10, 0x03}),
                                                RequestTo(0x7B0, {0x10, 0x03}), RequestTo(0x7B0, {0x85, 0x02}),
                                                RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7B0, {0x85, 0x02}),
                                                RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7DF, {0x28, 0x03, 0x01})};
    for (const bytes::Bytes& request : requests)
    {
        f.transport.Exchange(request);
        f.transport.QueueNoFrame();
    }

    EXPECT_THAT(ShInCarFireAndForget(f.ctx, f.transport, 2000ms), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(),
                ::testing::ElementsAre(200ms, 2000ms, 200ms, 200ms, 200ms, 200ms, 200ms, 200ms, 200ms, 200ms));
}

// A cancelled opening wait sends nothing; an absent probe reply aborts before the run.
TEST(DensoIso15765CanCommon, ConnectInCarStopsOnCancelledWaitOrAbsentProbe)
{
    for (const bool cancelled : {false, true})
    {
        SCOPED_TRACE(cancelled);
        CommonFixture f;
        if (cancelled)
        {
            f.cancellation.Cancel();
        }
        else
        {
            f.transport.Exchange(Request({0x10, 0x5F}));
            f.transport.QueueNoFrame();
        }

        EXPECT_THAT(DensoIso15765ConnectInCar(f.ctx, f.transport, DensoInCarRun::kSh, 2000ms),
                    fastecu::testing::IsErr(cancelled ? ErrorKind::kCancelled : ErrorKind::kTimeout));

        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_EQ(f.transport.write_attempts, cancelled ? 0U : 1U);
        EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(500ms));
        if (!cancelled)
        {
            EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms));
        }
    }
}

// N83M 1.5M's bench arm: a 500 ms settle, a fatal session read at 200 ms,
// then seed/key and every kernel-jump read at 200 ms.
constexpr DensoBenchArm kFatalBenchArm{
    .settle = 500ms,
    .session_reply = DensoBenchSessionReply::kFatal,
    .session_timeout = 200ms,
    .jump_first_read = 200ms,
    .jump_loop_read = 200ms,
    .discard_first_reply = false,
};

// A cancelled opening wait sends nothing; a wrong bench session reply is fatal before seed/key.
TEST(DensoIso15765CanCommon, ConnectBenchStopsOnCancelledWaitOrWrongSession)
{
    for (const bool cancelled : {false, true})
    {
        SCOPED_TRACE(cancelled);
        CommonFixture f;
        if (cancelled)
        {
            f.cancellation.Cancel();
        }
        else
        {
            f.transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x44}));
        }

        EXPECT_THAT(DensoIso15765ConnectBench(f.ctx, kFatalBenchArm),
                    fastecu::testing::IsErr(cancelled ? ErrorKind::kCancelled : ErrorKind::kBadResponse));

        EXPECT_TRUE(f.transport.ScriptConsumed());
        EXPECT_EQ(f.transport.write_attempts, cancelled ? 0U : 1U);
        EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(500ms));
        if (!cancelled)
        {
            EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(200ms));
        }
    }
}

// SH72543 diesel's bench arm: a 50 ms settle, a tolerant session probe read at
// 2000 ms, then seed/key and a kernel jump read at 2000/200 ms.
constexpr DensoBenchArm kTolerantBenchArm{
    .settle = 50ms,
    .session_reply = DensoBenchSessionReply::kTolerantProbe,
    .session_timeout = 2000ms,
    .jump_first_read = 2000ms,
    .jump_loop_read = 200ms,
    .discard_first_reply = false,
};

// A wrong reply to a tolerant bench session is logged and stepped over: seed/key follows.
TEST(DensoIso15765CanCommon, ConnectBenchTolerantProbeStepsOverWrongSessionReply)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x44}));
    f.transport.Exchange(Request({0x27, 0x61}));
    f.transport.QueueNoFrame();

    EXPECT_THAT(DensoIso15765ConnectBench(f.ctx, kTolerantBenchArm), fastecu::testing::IsErr(ErrorKind::kTimeout));

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.write_attempts, 2U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 2000ms));
    EXPECT_THAT(f.events.logs,
                ::testing::Contains(::testing::Pair(LogLevel::kError, "Wrong response from ECU: 50 44 ")));
}

// An absent reply to a tolerant bench session aborts before seed/key.
TEST(DensoIso15765CanCommon, ConnectBenchTolerantProbeAbortsOnAbsentSessionReply)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x10, 0x43}));
    f.transport.QueueNoFrame();

    EXPECT_THAT(DensoIso15765ConnectBench(f.ctx, kTolerantBenchArm), fastecu::testing::IsErr(ErrorKind::kTimeout));

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.write_attempts, 1U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms));
}

TEST(DensoIso15765CanCommon, ConnectBenchSettlesForTheArmsDelay)
{
    CommonFixture f;
    f.cancellation.Cancel();

    EXPECT_THAT(DensoIso15765ConnectBench(f.ctx, kTolerantBenchArm), fastecu::testing::IsErr(ErrorKind::kCancelled));

    EXPECT_EQ(f.transport.write_attempts, 0U);
    EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(50ms));
}

// Fails the `fail_write_at`-th write or `fail_read_at`-th read (1-based) with
// kDisconnected "injected", or cancels `token` as the `cancel_after_read`-th
// read returns; everything else follows the script.
class FaultingCanTransport final : public ScriptedCanFlashTransport
{
  public:
    explicit FaultingCanTransport(ManualCancellationToken& token) : token_(token)
    {
    }
    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        if (++writes_ == fail_write_at)
        {
            writes_at_fault = WritesConsumed();
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedCanFlashTransport::Write(data, cancellation);
    }
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        if (++reads_ == fail_read_at)
        {
            writes_at_fault = WritesConsumed();
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        auto read = ScriptedCanFlashTransport::Read(timeout, cancellation);
        if (reads_ == cancel_after_read)
        {
            token_.Cancel();
        }
        return read;
    }
    std::size_t Writes() const
    {
        return writes_;
    }
    std::size_t Reads() const
    {
        return reads_;
    }

    std::size_t fail_write_at = 0;
    std::size_t fail_read_at = 0;
    std::size_t cancel_after_read = 0;
    std::optional<std::size_t> writes_at_fault;

  private:
    ManualCancellationToken& token_;
    std::size_t writes_ = 0;
    std::size_t reads_ = 0;
};

struct FaultFixture
{
    ManualCancellationToken cancellation;
    FaultingCanTransport transport{cancellation};
    CanFlashUdsChannel channel{transport, 0x7E0, 0x7E8};
    RecordingClock clock;
    RecordingEventSink events;
    uds::UdsClient client{channel, clock, events};
    CanExecutorContext ctx{cancellation, events, clock, client, channel};
};

TEST(DensoIso15765CanCommon, JumpToKernelPropagatesSendAndDiscardSettleFailures)
{
    {
        FaultFixture f;
        f.transport.fail_write_at = 1;
        EXPECT_THAT(DensoIso15765JumpToKernel(f.ctx, 0x42, 50, 200ms, 200ms, /*discard_first_reply=*/true),
                    fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
        EXPECT_EQ(f.transport.Reads(), 0U);
    }
    {
        FaultFixture f;
        f.transport.Exchange(Request({0x10, 0x42}));
        f.transport.QueueNoFrame();
        f.transport.cancel_after_read = 1;
        EXPECT_THAT(DensoIso15765JumpToKernel(f.ctx, 0x42, 50, 200ms, 200ms, /*discard_first_reply=*/true),
                    fastecu::testing::IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(f.transport.Reads(), 1U); // the discard re-read never happens
    }
}

TEST(DensoIso15765CanCommon, ObkProbeSendFailurePropagates)
{
    FaultFixture f;
    f.transport.fail_write_at = 1;
    EXPECT_THAT(DensoObkIsActive(f.ctx, "OBK active"),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
    EXPECT_EQ(f.transport.Reads(), 0U);
}

TEST(DensoIso15765CanCommon, ProgrammingBranchSelectorShorterThanFourBytesIsBadResponse)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    f.transport.Exchange(Request({0x22, 0x10, 0x1D}), Response({0x62, 0x10, 0x1D}));

    EXPECT_THAT(DensoProgrammingBranchIsInCar(f.ctx, 200ms),
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse,
                                            ::testing::Eq("programming-branch selector reply is too short")));
    EXPECT_THAT(f.events.logs,
                ::testing::Contains(::testing::Pair(
                    LogLevel::kError, "Wrong response from ECU: programming-branch selector reply is too short")));
    EXPECT_TRUE(f.transport.ScriptConsumed());
}

TEST(DensoIso15765CanCommon, EraseTriggerSendFailureStopsBeforeTheSettle)
{
    FaultFixture f;
    ScriptEraseSetup(f.transport);
    f.transport.fail_write_at = 2;
    EXPECT_THAT(DensoIso15765Erase(f.ctx, kEraseRegion),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::IsEmpty());
}

// The SH in-car connect end to end: probe, the ten-exchange run, seed/key,
// session and selector confirmations, then an acknowledged jump.
void ScriptShInCarConnect(ScriptedCanFlashTransport& transport)
{
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x01}));
    for (const bytes::Bytes& request :
         {RequestTo(0x7A2, {0x10, 0xC0}), RequestTo(0x7E0, {0x10, 0x63}), RequestTo(0x7DF, {0x10, 0x03}),
          RequestTo(0x7E1, {0x10, 0x03}), RequestTo(0x7B0, {0x10, 0x03}), RequestTo(0x7B0, {0x85, 0x02}),
          RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7B0, {0x85, 0x02}), RequestTo(0x7DF, {0x85, 0x02}),
          RequestTo(0x7DF, {0x28, 0x03, 0x01})})
    {
        transport.Exchange(request);
        transport.QueueNoFrame();
    }
    transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    transport.Exchange(Request({0x10, 0x5F}), Response({0x50, 0x63}));
    transport.Exchange(Request({0x22, 0x10, 0x1D}), Response({0x62, 0x10}));
    transport.Exchange(Request({0x10, 0x62}), Response({0x50, 0x62}));
}

TEST(DensoIso15765CanCommon, EveryTransportFaultInAnInCarConnectPropagates)
{
    std::size_t writes = 0;
    std::size_t reads = 0;
    {
        FaultFixture f;
        ScriptShInCarConnect(f.transport);
        ASSERT_THAT(DensoIso15765ConnectInCar(f.ctx, f.transport, DensoInCarRun::kSh, 2000ms),
                    fastecu::testing::IsOk());
        ASSERT_TRUE(f.transport.ScriptConsumed());
        writes = f.transport.Writes();
        reads = f.transport.Reads();
    }
    for (const bool fail_read : {false, true})
    {
        SCOPED_TRACE(fail_read ? "read" : "write");
        for (std::size_t k = 1; k <= (fail_read ? reads : writes); ++k)
        {
            SCOPED_TRACE(k);
            FaultFixture f;
            ScriptShInCarConnect(f.transport);
            (fail_read ? f.transport.fail_read_at : f.transport.fail_write_at) = k;
            EXPECT_THAT(DensoIso15765ConnectInCar(f.ctx, f.transport, DensoInCarRun::kSh, 2000ms),
                        fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
            EXPECT_EQ(f.transport.writes_at_fault, std::optional<std::size_t>{f.transport.WritesConsumed()});
        }
    }
}

} // namespace
} // namespace fastecu::flash
