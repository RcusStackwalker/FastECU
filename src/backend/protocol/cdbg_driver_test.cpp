#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>
#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/protocol/testing/scripted_can_transport.h"
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
    ASSERT_EQ(result->Size(), 2U);
    ASSERT_EQ(result->At(0), std::uint32_t(42));
    ASSERT_EQ(result->At(1), std::uint32_t(0x1234));
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

TEST(TestCdbgDriver, poll_merges_values_across_two_frames)
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
    ASSERT_EQ(r1->Size(), 3U);
    ASSERT_EQ(r1->At(0), std::uint32_t(0xAABBCCDD));
    ASSERT_EQ(r1->At(1), std::uint32_t(0));
    ASSERT_EQ(r1->At(2), std::uint32_t(0));

    // Frame 1 arrives next: channel 1 (4-byte) = 0x11223344, channel 2 (2-byte) = 0x5566.
    t.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0111223344556600"));
    const auto r2 = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(r2, fastecu::testing::IsOk());
    ASSERT_EQ(r2->Size(), 3U);
    ASSERT_EQ(r2->At(0), std::uint32_t(0xAABBCCDD)); // retained from frame 0
    ASSERT_EQ(r2->At(1), std::uint32_t(0x11223344));
    ASSERT_EQ(r2->At(2), std::uint32_t(0x5566));
}

TEST(TestCdbgDriver, poll_returns_empty_when_not_streaming)
{
    cdbg::ScriptedCanTransport t;
    CdbgLogDriver d(t);
    fastecu::FakeCancellationToken cancellation;
    const auto result = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->Empty());
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
