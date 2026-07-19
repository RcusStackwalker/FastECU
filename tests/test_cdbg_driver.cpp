#include <gtest/gtest.h>
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include "byte_test_utils.h"
#include "scripted_can_transport.h"
using namespace MitsuColtCanCdbg;

TEST(TestCdbgDriver, handshake_and_single_frame_streaming)
{
    cdbg::ScriptedCanTransport t;
    QVector<CdbgChannel> ch = {{0x804FBF, 1}, {0x804DF2, 2}};

    t.expectWrite(kRequestCanId, buildInitFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

    t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000012345678")); // seed=0x12345678

    t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33));        // seedToKey(0x12345678)
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000100000000")); // byte3 != 0 -> granted

    t.expectWrite(kRequestCanId, buildLogResetFrame(0));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

    QVector<QVector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(ch, frames));
    ASSERT_EQ(frames.size(), 1);
    for (const CdbgFrame& cmd : buildFrameInitFrames(0, 0, frames.at(0)))
    {
        t.expectWrite(kRequestCanId, cmd);
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
    }

    t.expectWrite(kRequestCanId, buildLogStartFrame(0, 1, 10));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

    CdbgLogDriver d(t);
    ASSERT_TRUE(d.startFreeFormLog(ch, 0, 10));
    ASSERT_TRUE(d.isStreaming());
    ASSERT_TRUE(t.scriptConsumed());
    ASSERT_TRUE(t.ok());

    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("002A123400000000"));
    QVector<std::uint32_t> vals = d.pollOnce(50);
    ASSERT_EQ(vals.size(), 2);
    ASSERT_EQ(vals.at(0), std::uint32_t(42));
    ASSERT_EQ(vals.at(1), std::uint32_t(0x1234));
}

TEST(TestCdbgDriver, accepts_live_security_reply_shape)
{
    cdbg::ScriptedCanTransport t;
    QVector<CdbgChannel> ch = {{0x804FBF, 1}};

    t.expectWrite(kRequestCanId, buildInitFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF0001FE00000000"));
    t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF000001D61B2EEA"));
    t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0xBA80A2C1));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF000002D61B2EEA"));
    t.expectWrite(kRequestCanId, buildLogResetFrame(0));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF00000000000000"));

    QVector<QVector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(ch, frames));
    for (const CdbgFrame& cmd : buildFrameInitFrames(0, 0, frames.at(0)))
    {
        t.expectWrite(kRequestCanId, cmd);
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF00000000000000"));
    }
    t.expectWrite(kRequestCanId, buildLogStartFrame(0, 1, 10));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF00000000000000"));

    CdbgLogDriver d(t);
    QString error;
    ASSERT_TRUE(d.startFreeFormLog(ch, 0, 10, &error));
    ASSERT_TRUE(error.isEmpty());
    ASSERT_TRUE(d.isStreaming());
    ASSERT_TRUE(t.scriptConsumed());
    ASSERT_TRUE(t.ok());
}

TEST(TestCdbgDriver, fails_before_handshake_when_no_channels_selected)
{
    cdbg::ScriptedCanTransport t;
    CdbgLogDriver d(t);
    QString error;

    ASSERT_TRUE(!d.startFreeFormLog({}, 0, 10, &error));
    ASSERT_EQ(error, QString("no CDBG log parameters selected"));
    ASSERT_TRUE(!d.isStreaming());
    ASSERT_TRUE(t.scriptConsumed());
}

TEST(TestCdbgDriver, handshake_fails_when_security_not_granted)
{
    cdbg::ScriptedCanTransport t;
    QVector<CdbgChannel> ch = {{0x804FBF, 1}};

    t.expectWrite(kRequestCanId, buildInitFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
    t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000")); // seed=0
    t.expectWrite(kRequestCanId, buildSecurityKeyFrame(seedToKey(0)));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000")); // byte3 == 0 -> denied

    CdbgLogDriver d(t);
    ASSERT_TRUE(!d.startFreeFormLog(ch, 0, 10));
    ASSERT_TRUE(!d.isStreaming());
}

TEST(TestCdbgDriver, handshake_fails_when_init_gets_no_reply)
{
    cdbg::ScriptedCanTransport t;
    QVector<CdbgChannel> ch = {{0x804FBF, 1}};
    t.expectWrite(kRequestCanId, buildInitFrame());
    // no queued read -> transport returns empty payload -> handshake must fail here.
    CdbgLogDriver d(t);
    ASSERT_TRUE(!d.startFreeFormLog(ch, 0, 10));
    ASSERT_TRUE(!d.isStreaming());
}

TEST(TestCdbgDriver, poll_merges_values_across_two_frames)
{
    cdbg::ScriptedCanTransport t;
    QVector<CdbgChannel> ch = {{0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2}};

    t.expectWrite(kRequestCanId, buildInitFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
    t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000012345678"));
    t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000100000000"));
    t.expectWrite(kRequestCanId, buildLogResetFrame(0));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

    QVector<QVector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(ch, frames));
    ASSERT_EQ(frames.size(), 2);
    for (int f = 0; f < frames.size(); ++f)
    {
        for (const CdbgFrame& cmd : buildFrameInitFrames(0, bytes::Byte(f), frames.at(f)))
        {
            t.expectWrite(kRequestCanId, cmd);
            t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
        }
    }
    t.expectWrite(kRequestCanId, buildLogStartFrame(0, 2, 10));
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

    CdbgLogDriver d(t);
    ASSERT_TRUE(d.startFreeFormLog(ch, 0, 10));

    // Frame 0 arrives first: channel 0 (4-byte) = 0xAABBCCDD.
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("00AABBCCDD000000"));
    QVector<std::uint32_t> v1 = d.pollOnce(50);
    ASSERT_EQ(v1.size(), 3);
    ASSERT_EQ(v1.at(0), std::uint32_t(0xAABBCCDD));
    ASSERT_EQ(v1.at(1), std::uint32_t(0));
    ASSERT_EQ(v1.at(2), std::uint32_t(0));

    // Frame 1 arrives next: channel 1 (4-byte) = 0x11223344, channel 2 (2-byte) = 0x5566.
    t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0111223344556600"));
    QVector<std::uint32_t> v2 = d.pollOnce(50);
    ASSERT_EQ(v2.size(), 3);
    ASSERT_EQ(v2.at(0), std::uint32_t(0xAABBCCDD)); // retained from frame 0
    ASSERT_EQ(v2.at(1), std::uint32_t(0x11223344));
    ASSERT_EQ(v2.at(2), std::uint32_t(0x5566));
}

TEST(TestCdbgDriver, poll_returns_empty_when_not_streaming)
{
    cdbg::ScriptedCanTransport t;
    CdbgLogDriver d(t);
    ASSERT_TRUE(d.pollOnce(50).isEmpty());
}
