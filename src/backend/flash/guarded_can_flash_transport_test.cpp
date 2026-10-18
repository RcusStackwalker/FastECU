#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/guarded_can_flash_transport.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>

#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"

namespace
{
using namespace std::chrono_literals;

using bytes::Bytes;
using fastecu::ErrorKind;
using fastecu::ManualCancellationToken;
using fastecu::flash::GuardedCanFlashTransport;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::ScriptedTransportInitialState;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

TEST(GuardedCanFlashTransportTest, ACancelledWriteNeverReachesTheTransport)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    ManualCancellationToken never;
    inner.ExpectWrite(Bytes{0x01});
    GuardedCanFlashTransport guarded(inner, cancellation);

    cancellation.Cancel();

    EXPECT_THAT(guarded.Write(Bytes{0x01}, never), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(inner.WritesConsumed(), 0U);
}

TEST(GuardedCanFlashTransportTest, ACancelBetweenRequestAndReplyStopsBeforeTheReply)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    ManualCancellationToken never;
    inner.Exchange(Bytes{0x01}, Bytes{0x02});
    GuardedCanFlashTransport guarded(inner, cancellation);

    EXPECT_THAT(guarded.Write(Bytes{0x01}, never), IsOk());
    cancellation.Cancel();

    EXPECT_THAT(guarded.Read(100ms, never), IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(inner.ReadTimeouts().empty());
}

TEST(GuardedCanFlashTransportTest, TransportErrorsPassThroughUnchanged)
{
    for (const ErrorKind kind : {ErrorKind::kTimeout, ErrorKind::kDisconnected, ErrorKind::kBadResponse})
    {
        ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
        ManualCancellationToken cancellation;
        inner.QueueError(kind);
        GuardedCanFlashTransport guarded(inner, cancellation);

        EXPECT_THAT(guarded.Read(100ms, cancellation), IsErr(kind));
    }
}

TEST(GuardedCanFlashTransportTest, NoFramePassesThrough)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    inner.QueueNoFrame();
    GuardedCanFlashTransport guarded(inner, cancellation);

    const auto frame = guarded.Read(100ms, cancellation);

    ASSERT_THAT(frame, IsOk());
    EXPECT_EQ(*frame, std::nullopt);
}

TEST(GuardedCanFlashTransportTest, CloseIsNeverRefused)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    GuardedCanFlashTransport guarded(inner, cancellation);

    cancellation.Cancel();

    EXPECT_THAT(guarded.Close(), IsOk());
    EXPECT_EQ(inner.close_call_count, 1);
}

TEST(GuardedCanFlashTransportTest, ACriticalSectionDefersCancelToTheNextIo)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    ManualCancellationToken never;
    inner.Exchange(Bytes{0x01}, Bytes{0x02});
    inner.ExpectWrite(Bytes{0x03});
    GuardedCanFlashTransport guarded(inner, cancellation);

    {
        const auto section = guarded.EnterCriticalSection();
        cancellation.Cancel();
        EXPECT_THAT(guarded.Write(Bytes{0x01}, never), IsOk());
        const auto reply = guarded.Read(100ms, never);
        ASSERT_THAT(reply, IsOk());
        EXPECT_EQ(*reply, std::optional<Bytes>(Bytes{0x02}));
    }

    EXPECT_THAT(guarded.Write(Bytes{0x03}, never), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(inner.WritesConsumed(), 1U);
}

TEST(GuardedCanFlashTransportTest, NestedCriticalSectionsDeferUntilTheOutermostEnds)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    ManualCancellationToken never;
    inner.ExpectWrite(Bytes{0x01});
    inner.ExpectWrite(Bytes{0x02});
    GuardedCanFlashTransport guarded(inner, cancellation);

    std::optional<GuardedCanFlashTransport::CriticalSection> outer = guarded.EnterCriticalSection();
    cancellation.Cancel();
    {
        const auto nested = guarded.EnterCriticalSection();
        EXPECT_THAT(guarded.Write(Bytes{0x01}, never), IsOk());
    }
    EXPECT_THAT(guarded.Write(Bytes{0x02}, never), IsOk());

    // Destroying the outer section is the only thing that re-arms the guard.
    outer.reset();
    EXPECT_THAT(guarded.Write(Bytes{0x03}, never), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(inner.WritesConsumed(), 2U);
}

TEST(GuardedCanFlashTransportTest, RequestUnblockReachesTheTransport)
{
    ScriptedCanFlashTransport inner(ScriptedTransportInitialState::kOpen);
    ManualCancellationToken cancellation;
    inner.QueueBlockingRead();
    GuardedCanFlashTransport guarded(inner, cancellation);

    auto pending = std::async(std::launch::async, [&] { return guarded.Read(5000ms, cancellation); });
    guarded.RequestUnblock();

    ASSERT_EQ(pending.wait_for(5s), std::future_status::ready);
    EXPECT_THAT(pending.get(), IsErr(ErrorKind::kCancelled));
}

} // namespace
