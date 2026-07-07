#include <QtTest>
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include "byte_test_utils.h"
#include "scripted_can_transport.h"
#include "test_cdbg_driver.h"
using namespace MitsuColtCanCdbg;

class TestCdbgDriver : public QObject { Q_OBJECT
private slots:
    void handshake_and_single_frame_streaming() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1}, {0x804DF2, 2} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000012345678")); // seed=0x12345678

        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33)); // seedToKey(0x12345678)
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000100000000")); // byte3 != 0 -> granted

        t.expectWrite(kRequestCanId, buildLogResetFrame(0));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(ch, frames));
        QCOMPARE(frames.size(), 1);
        for (const CdbgFrame &cmd : buildFrameInitFrames(0, 0, frames.at(0))) {
            t.expectWrite(kRequestCanId, cmd);
            t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
        }

        t.expectWrite(kRequestCanId, buildLogStartFrame(0, 1, 10));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

        CdbgLogDriver d(t);
        QVERIFY(d.startFreeFormLog(ch, 0, 10));
        QVERIFY(d.isStreaming());
        QVERIFY(t.scriptConsumed());
        QVERIFY(t.ok());

        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("002A123400000000"));
        QVector<quint32> vals = d.pollOnce(50);
        QCOMPARE(vals.size(), 2);
        QCOMPARE(vals.at(0), quint32(42));
        QCOMPARE(vals.at(1), quint32(0x1234));
    }

    void accepts_live_security_reply_shape() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF0001FE00000000"));
        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF000001D61B2EEA"));
        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0xBA80A2C1));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF000002D61B2EEA"));
        t.expectWrite(kRequestCanId, buildLogResetFrame(0));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF00000000000000"));

        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(ch, frames));
        for (const CdbgFrame &cmd : buildFrameInitFrames(0, 0, frames.at(0))) {
            t.expectWrite(kRequestCanId, cmd);
            t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF00000000000000"));
        }
        t.expectWrite(kRequestCanId, buildLogStartFrame(0, 1, 10));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("FF00000000000000"));

        CdbgLogDriver d(t);
        QString error;
        QVERIFY(d.startFreeFormLog(ch, 0, 10, &error));
        QVERIFY(error.isEmpty());
        QVERIFY(d.isStreaming());
        QVERIFY(t.scriptConsumed());
        QVERIFY(t.ok());
    }

    void fails_before_handshake_when_no_channels_selected() {
        cdbg::ScriptedCanTransport t;
        CdbgLogDriver d(t);
        QString error;

        QVERIFY(!d.startFreeFormLog({}, 0, 10, &error));
        QCOMPARE(error, QString("no CDBG log parameters selected"));
        QVERIFY(!d.isStreaming());
        QVERIFY(t.scriptConsumed());
    }

    void handshake_fails_when_security_not_granted() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000")); // seed=0
        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(seedToKey(0)));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000")); // byte3 == 0 -> denied

        CdbgLogDriver d(t);
        QVERIFY(!d.startFreeFormLog(ch, 0, 10));
        QVERIFY(!d.isStreaming());
    }

    void handshake_fails_when_init_gets_no_reply() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1} };
        t.expectWrite(kRequestCanId, buildInitFrame());
        // no queued read -> transport returns empty payload -> handshake must fail here.
        CdbgLogDriver d(t);
        QVERIFY(!d.startFreeFormLog(ch, 0, 10));
        QVERIFY(!d.isStreaming());
    }

    void poll_merges_values_across_two_frames() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000012345678"));
        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000100000000"));
        t.expectWrite(kRequestCanId, buildLogResetFrame(0));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(ch, frames));
        QCOMPARE(frames.size(), 2);
        for (int f = 0; f < frames.size(); ++f) {
            for (const CdbgFrame &cmd : buildFrameInitFrames(0, quint8(f), frames.at(f))) {
                t.expectWrite(kRequestCanId, cmd);
                t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
            }
        }
        t.expectWrite(kRequestCanId, buildLogStartFrame(0, 2, 10));
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

        CdbgLogDriver d(t);
        QVERIFY(d.startFreeFormLog(ch, 0, 10));

        // Frame 0 arrives first: channel 0 (4-byte) = 0xAABBCCDD.
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("00AABBCCDD000000"));
        QVector<quint32> v1 = d.pollOnce(50);
        QCOMPARE(v1.size(), 3);
        QCOMPARE(v1.at(0), quint32(0xAABBCCDD));
        QCOMPARE(v1.at(1), quint32(0));
        QCOMPARE(v1.at(2), quint32(0));

        // Frame 1 arrives next: channel 1 (4-byte) = 0x11223344, channel 2 (2-byte) = 0x5566.
        t.queueRead(kReplyCanId, test_bytes::bytesFromHex("0111223344556600"));
        QVector<quint32> v2 = d.pollOnce(50);
        QCOMPARE(v2.size(), 3);
        QCOMPARE(v2.at(0), quint32(0xAABBCCDD)); // retained from frame 0
        QCOMPARE(v2.at(1), quint32(0x11223344));
        QCOMPARE(v2.at(2), quint32(0x5566));
    }

    void poll_returns_empty_when_not_streaming() {
        cdbg::ScriptedCanTransport t;
        CdbgLogDriver d(t);
        QVERIFY(d.pollOnce(50).isEmpty());
    }
};
int run_test_cdbg_driver(int argc, char** argv) {
    TestCdbgDriver t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_cdbg_driver.moc"
