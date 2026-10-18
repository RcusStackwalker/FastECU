#include "src/backend/flash/testing/transcript_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using Kind = WireTranscriptStep::Kind;

const bytes::Bytes kReq1{0x01, 0x02};
const bytes::Bytes kResp1{0x41, 0x02};
const bytes::Bytes kReq2{0x03};

WireTranscriptStep Step(Kind kind, bytes::Bytes frame = {}, std::chrono::milliseconds d = {}, std::string label = {})
{
    return WireTranscriptStep{kind, std::move(frame), d, std::move(label)};
}

WireTranscript Recorded()
{
    ScriptedCanFlashTransport inner;
    FakeClock clock;
    FakeCancellationToken cancel;
    {
        auto a = inner.Section("a");
        inner.Exchange(kReq1, kResp1);
    }
    {
        auto b = inner.Section("b");
        inner.ExpectWrite(kReq2);
        inner.QueueNoFrame();
    }
    RecordingTranscriptCanFlashTransport rec(inner, clock);
    EXPECT_THAT(rec.Write(kReq1, cancel), IsOk());
    EXPECT_THAT(rec.Read(1000ms, cancel), IsOk());
    EXPECT_THAT(rec.Write(kReq2, cancel), IsOk());
    EXPECT_THAT(rec.Read(500ms, cancel), IsOk());
    return rec.Transcript();
}

TEST(TranscriptCanFlashTransport, RecordsWritesReadsSilenceTimeoutsAndSections)
{
    const WireTranscript expected{Step(Kind::kSection, {}, {}, "a"),    Step(Kind::kWrite, kReq1),
                                  Step(Kind::kReadTimeout, {}, 1000ms), Step(Kind::kRead, kResp1),
                                  Step(Kind::kSection, {}, {}, "b"),    Step(Kind::kWrite, kReq2),
                                  Step(Kind::kReadTimeout, {}, 500ms),  Step(Kind::kNoFrame)};
    EXPECT_EQ(Recorded(), expected);
}

TEST(TranscriptCanFlashTransport, RecordsSleepAndForwardsClock)
{
    ScriptedCanFlashTransport inner;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingTranscriptCanFlashTransport rec(inner, clock);
    EXPECT_THAT(rec.Sleep(20ms, cancel), IsOk());
    EXPECT_EQ(clock.Elapsed(), 20ms);
    EXPECT_EQ(rec.Transcript(), WireTranscript{Step(Kind::kSleep, {}, 20ms)});
}

TEST(TranscriptCanFlashTransport, ReplayAcceptsTheRecordedRun)
{
    ReplayCanFlashTransport replay(Recorded());
    FakeCancellationToken cancel;
    EXPECT_FALSE(replay.Consumed());
    EXPECT_THAT(replay.Write(kReq1, cancel), IsOk());
    auto r1 = replay.Read(1000ms, cancel);
    ASSERT_THAT(r1, IsOk());
    EXPECT_EQ(**r1, kResp1);
    EXPECT_THAT(replay.Write(kReq2, cancel), IsOk());
    auto r2 = replay.Read(500ms, cancel);
    ASSERT_THAT(r2, IsOk());
    EXPECT_FALSE(r2->has_value());
    EXPECT_TRUE(replay.Consumed());
}

TEST(TranscriptCanFlashTransport, ReplayRejectsADivergentWrite)
{
    ReplayCanFlashTransport replay(Recorded());
    FakeCancellationToken cancel;
    bytes::Bytes bad = kReq1;
    bad[0] ^= 0x01;
    const Status status = replay.Write(bad, cancel);
    EXPECT_THAT(status, IsErr(ErrorKind::kInternal));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("step 1"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("section a"));
}

TEST(TranscriptCanFlashTransport, ReplayRejectsAChangedReadTimeout)
{
    ReplayCanFlashTransport replay(Recorded());
    FakeCancellationToken cancel;
    ASSERT_THAT(replay.Write(kReq1, cancel), IsOk());
    EXPECT_THAT(replay.Read(999ms, cancel), IsErr(ErrorKind::kInternal));
}

TEST(TranscriptCanFlashTransport, ReplayRejectsAnUnrecordedSleep)
{
    ReplayCanFlashTransport replay(Recorded());
    FakeCancellationToken cancel;
    EXPECT_THAT(replay.Sleep(20ms, cancel), IsErr(ErrorKind::kInternal));
}

TEST(TranscriptCanFlashTransport, ReplaySleepAdvancesNowAndHonorsCancellation)
{
    ReplayCanFlashTransport replay({Step(Kind::kSleep, {}, 20ms), Step(Kind::kSleep, {}, 5ms)});
    FakeCancellationToken cancel;
    const auto start = replay.Now();
    EXPECT_THAT(replay.Sleep(20ms, cancel), IsOk());
    EXPECT_EQ(replay.Now() - start, 20ms);
    cancel.SetCancelled(true);
    EXPECT_THAT(replay.Sleep(5ms, cancel), IsErr(ErrorKind::kCancelled));
}

} // namespace
} // namespace fastecu::flash
