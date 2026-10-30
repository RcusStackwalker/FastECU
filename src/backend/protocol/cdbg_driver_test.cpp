#include "src/backend/ports/testing/result_matchers.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/protocol/testing/scripted_can_transport.h"

#include <ranges>
#include <string>
#include <tuple>
#include <vector>

using ::testing::IsEmpty;
using namespace mitsu_colt_can_cdbg;
using namespace std::chrono_literals;

TEST(TestCdbgDriver, handshake_and_single_frame_streaming)
{
    cdbg::ScriptedCanTransport t;
    std::vector<CdbgChannel> ch = {{0x804FBF, 1}, {0x804DF2, 2}};

    t.ExpectWrite(kRequestCanId, BuildInitFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    t.ExpectWrite(kRequestCanId, BuildSecuritySeedRequestFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000012345678")); // seed=0x12345678

    t.ExpectWrite(kRequestCanId, BuildSecurityKeyFrame(0x8C536B33));        // seedToKey(0x12345678)
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000100000000")); // byte3 != 0 -> granted

    t.ExpectWrite(kRequestCanId, BuildLogResetFrame(0));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(BatchChannelsIntoFrames(ch, frames));
    ASSERT_EQ(frames.size(), 1U);
    for (const CdbgFrame& cmd : BuildFrameInitFrames(0, 0, frames.at(0)))
    {
        t.ExpectWrite(kRequestCanId, cmd);
        t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    }

    t.ExpectWrite(kRequestCanId, BuildLogStartFrame(0, 1, 10));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsOk());
    ASSERT_TRUE(d.IsStreaming());
    ASSERT_TRUE(t.ScriptConsumed());
    ASSERT_TRUE(t.Ok());

    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("002A123400000000"));
    const auto result = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->values.size(), 2U);
    ASSERT_EQ(result->values.at(0), std::uint32_t(42));
    ASSERT_EQ(result->values.at(1), std::uint32_t(0x1234));
}

TEST(TestCdbgDriver, accepts_live_security_reply_shape)
{
    cdbg::ScriptedCanTransport t;
    std::vector<CdbgChannel> ch = {{0x804FBF, 1}};

    t.ExpectWrite(kRequestCanId, BuildInitFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("FF0001FE00000000"));
    t.ExpectWrite(kRequestCanId, BuildSecuritySeedRequestFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("FF000001D61B2EEA"));
    t.ExpectWrite(kRequestCanId, BuildSecurityKeyFrame(0xBA80A2C1));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("FF000002D61B2EEA"));
    t.ExpectWrite(kRequestCanId, BuildLogResetFrame(0));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("FF00000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(BatchChannelsIntoFrames(ch, frames));
    for (const CdbgFrame& cmd : BuildFrameInitFrames(0, 0, frames.at(0)))
    {
        t.ExpectWrite(kRequestCanId, cmd);
        t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("FF00000000000000"));
    }
    t.ExpectWrite(kRequestCanId, BuildLogStartFrame(0, 1, 10));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("FF00000000000000"));

    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsOk());
    ASSERT_TRUE(d.IsStreaming());
    ASSERT_TRUE(t.ScriptConsumed());
    ASSERT_TRUE(t.Ok());
}

TEST(TestCdbgDriver, fails_before_handshake_when_no_channels_selected)
{
    cdbg::ScriptedCanTransport t;
    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(d.StartFreeFormLog({}, 0, 10, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInvalidConfig, "no CDBG log parameters selected"));
    ASSERT_TRUE(!d.IsStreaming());
    ASSERT_TRUE(t.ScriptConsumed());
}

TEST(TestCdbgDriver, handshake_fails_when_security_not_granted)
{
    cdbg::ScriptedCanTransport t;
    std::vector<CdbgChannel> ch = {{0x804FBF, 1}};

    t.ExpectWrite(kRequestCanId, BuildInitFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    t.ExpectWrite(kRequestCanId, BuildSecuritySeedRequestFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000")); // seed=0
    t.ExpectWrite(kRequestCanId, BuildSecurityKeyFrame(SeedToKey(0)));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000")); // byte3 == 0 -> denied

    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
    ASSERT_TRUE(!d.IsStreaming());
}

TEST(TestCdbgDriver, handshake_fails_when_init_gets_no_reply)
{
    cdbg::ScriptedCanTransport t;
    std::vector<CdbgChannel> ch = {{0x804FBF, 1}};
    t.ExpectWrite(kRequestCanId, BuildInitFrame());
    t.QueueNoFrame();
    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
    ASSERT_TRUE(!d.IsStreaming());
}

TEST(TestCdbgDriver, poll_publishes_only_the_received_frame_without_unseen_zero_values)
{
    cdbg::ScriptedCanTransport t;
    std::vector<CdbgChannel> ch = {{0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2}};

    t.ExpectWrite(kRequestCanId, BuildInitFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    t.ExpectWrite(kRequestCanId, BuildSecuritySeedRequestFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000012345678"));
    t.ExpectWrite(kRequestCanId, BuildSecurityKeyFrame(0x8C536B33));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000100000000"));
    t.ExpectWrite(kRequestCanId, BuildLogResetFrame(0));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(BatchChannelsIntoFrames(ch, frames));
    ASSERT_EQ(frames.size(), 2U);
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        for (const CdbgFrame& cmd : BuildFrameInitFrames(0, bytes::Byte(f), frames.at(f)))
        {
            t.ExpectWrite(kRequestCanId, cmd);
            t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
        }
    }
    t.ExpectWrite(kRequestCanId, BuildLogStartFrame(0, 2, 10));
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsOk());

    // Frame 0 arrives first: channel 0 (4-byte) = 0xAABBCCDD.
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("00AABBCCDD000000"));
    const auto r1 = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(r1, fastecu::testing::IsOk());
    ASSERT_EQ(r1->values.size(), 1U);
    EXPECT_EQ(r1->channel_offset, 0U);
    ASSERT_EQ(r1->values.at(0), std::uint32_t(0xAABBCCDD));

    // Frame 1 arrives next: channel 1 (4-byte) = 0x11223344, channel 2 (2-byte) = 0x5566.
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0111223344556600"));
    const auto r2 = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(r2, fastecu::testing::IsOk());
    ASSERT_EQ(r2->values.size(), 2U);
    EXPECT_EQ(r2->channel_offset, 1U);
    ASSERT_EQ(r2->values.at(0), std::uint32_t(0x11223344));
    ASSERT_EQ(r2->values.at(1), std::uint32_t(0x5566));
}

TEST(TestCdbgDriver, poll_returns_empty_when_not_streaming)
{
    cdbg::ScriptedCanTransport t;
    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    const auto result = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->values, IsEmpty());
}

TEST(TestCdbgDriver, handshake_propagates_cancellation_from_bounded_read)
{
    cdbg::ScriptedCanTransport t;
    std::vector<CdbgChannel> ch = {{0x804FBF, 1}};
    t.ExpectWrite(kRequestCanId, BuildInitFrame());
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation(true);

    ASSERT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

namespace
{
struct HandshakeStep
{
    CdbgFrame write;
    bytes::Bytes reply;
    std::string detail;
};

// The successful StartFreeFormLog exchange for `ch`, one step per frame sent.
std::vector<HandshakeStep> HandshakeSteps(const std::vector<CdbgChannel>& ch)
{
    const bytes::Bytes ack = test_bytes::BytesFromHex("0000000000000000");
    std::vector<HandshakeStep> steps{
        {BuildInitFrame(), ack, "CDBG session init failed"},
        {BuildSecuritySeedRequestFrame(), test_bytes::BytesFromHex("0000000012345678"),
         "CDBG security seed request failed"},
        {BuildSecurityKeyFrame(0x8C536B33), test_bytes::BytesFromHex("0000000100000000"),
         "CDBG security key request failed"},
        {BuildLogResetFrame(0), ack, "CDBG log reset failed"},
    };
    std::vector<std::vector<CdbgChannel>> frames;
    std::ignore = BatchChannelsIntoFrames(ch, frames);
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        steps.append_range(BuildFrameInitFrames(0, bytes::Byte(f), frames.at(f)) |
                           std::views::transform([&](const CdbgFrame& cmd)
                                                 { return HandshakeStep{cmd, ack, "CDBG log frame setup failed"}; }));
    }
    steps.push_back({BuildLogStartFrame(0, static_cast<bytes::Byte>(frames.size()), 10), ack, "CDBG log start failed"});
    return steps;
}

enum class HandshakeFault
{
    kWriteRejected,
    kShortWrite,
    kReadError,
    kNoReply,
    kWrongId,
    kEmptyPayload,
};

// Scripts `steps` up to and including `faulted`, whose exchange fails as `fault` says.
void ScriptFault(cdbg::ScriptedCanTransport& t, const std::vector<HandshakeStep>& steps, std::size_t faulted,
                 HandshakeFault fault)
{
    for (std::size_t i = 0; i < faulted; ++i)
    {
        t.ExpectWrite(kRequestCanId, steps[i].write);
        t.QueueRead(kReplyCanId, steps[i].reply);
    }
    switch (fault)
    {
    case HandshakeFault::kWriteRejected:
        break;
    case HandshakeFault::kShortWrite:
        t.ExpectShortWrite(kRequestCanId, steps[faulted].write);
        break;
    case HandshakeFault::kReadError:
        t.ExpectWrite(kRequestCanId, steps[faulted].write);
        t.QueueError(fastecu::ErrorKind::kTimeout, "scripted read failure");
        break;
    case HandshakeFault::kNoReply:
        t.ExpectWrite(kRequestCanId, steps[faulted].write);
        t.QueueNoFrame();
        break;
    case HandshakeFault::kWrongId:
        t.ExpectWrite(kRequestCanId, steps[faulted].write);
        t.QueueRead(kRequestCanId, steps[faulted].reply);
        break;
    case HandshakeFault::kEmptyPayload:
        t.ExpectWrite(kRequestCanId, steps[faulted].write);
        t.QueueRead(kReplyCanId, bytes::Bytes{});
        break;
    }
}
} // namespace

TEST(TestCdbgDriver, handshake_propagates_each_exchange_failure_and_stays_idle)
{
    const std::vector<CdbgChannel> ch = {{0x804FBF, 1}, {0x804DF2, 2}};
    const std::vector<HandshakeStep> steps = HandshakeSteps(ch);
    for (std::size_t faulted = 0; faulted < steps.size(); ++faulted)
    {
        SCOPED_TRACE(::testing::Message() << "step " << faulted);
        for (const auto& [fault, kind, detail] :
             {std::tuple{HandshakeFault::kWriteRejected, fastecu::ErrorKind::kInternal,
                         std::string("unexpected scripted CAN write")},
              std::tuple{HandshakeFault::kShortWrite, fastecu::ErrorKind::kInternal, std::string("partial CAN write")},
              std::tuple{HandshakeFault::kReadError, fastecu::ErrorKind::kTimeout,
                         std::string("scripted read failure")},
              std::tuple{HandshakeFault::kNoReply, fastecu::ErrorKind::kBadResponse, steps[faulted].detail},
              std::tuple{HandshakeFault::kWrongId, fastecu::ErrorKind::kBadResponse, steps[faulted].detail},
              std::tuple{HandshakeFault::kEmptyPayload, fastecu::ErrorKind::kBadResponse, steps[faulted].detail}})
        {
            SCOPED_TRACE(::testing::Message() << "fault " << static_cast<int>(fault));
            cdbg::ScriptedCanTransport t;
            ScriptFault(t, steps, faulted, fault);
            CdbgLogDriver d(t);
            fastecu::FakeCancellationToken cancellation;

            EXPECT_THAT(d.StartFreeFormLog(ch, 0, 10, cancellation), fastecu::testing::IsErrWith(kind, detail));
            EXPECT_FALSE(d.IsStreaming());
            EXPECT_TRUE(t.ScriptConsumed());
        }
    }
}

TEST(TestCdbgDriver, rejects_channels_that_need_more_than_the_maximum_frames_after_unlocking)
{
    // A four-byte channel fills a frame, so one channel past the limit needs a ninth frame.
    const std::vector<CdbgChannel> ch(static_cast<std::size_t>(kMaxFrames) + 1, CdbgChannel{0x804FBF, 4});
    const std::vector<HandshakeStep> steps = HandshakeSteps({{0x804FBF, 1}});
    cdbg::ScriptedCanTransport t;
    for (std::size_t i = 0; i < 3; ++i)
    {
        t.ExpectWrite(kRequestCanId, steps[i].write);
        t.QueueRead(kReplyCanId, steps[i].reply);
    }
    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;

    EXPECT_THAT(
        d.StartFreeFormLog(ch, 0, 10, cancellation),
        fastecu::testing::IsErrWith(fastecu::ErrorKind::kInvalidConfig, "too many CDBG log parameters selected"));
    EXPECT_FALSE(d.IsStreaming());
    EXPECT_TRUE(t.ScriptConsumed());
}
