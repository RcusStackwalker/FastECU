#include <QtTest>
#include "protocol/mitsu_colt_can_cdbg_protocol.h"
#include "test_mitsu_colt_can_cdbg_protocol.h"
using namespace MitsuColtCanCdbg;

class TestMitsuColtCanCdbgProtocol : public QObject { Q_OBJECT
private slots:
    void init_frame_layout() {
        QCOMPARE(buildInitFrame(), QByteArray::fromHex("0101000000000000"));
    }
    void security_seed_request_frame_layout() {
        QCOMPARE(buildSecuritySeedRequestFrame(), QByteArray::fromHex("1200020000000000"));
    }
    void seed_to_key_matches_known_vectors_across_all_parity_branches() {
        // Hand-verified by simulating cdbgengine.cpp's seed_to_key() in Python
        // (per-byte increment keyed on low 2 bits, 8-bit rotate-left-3, then a
        // parity-keyed byte permutation, then two 16-bit multiply-accumulates).
        // Each case below exercises a different parity (0-4) branch.
        QCOMPARE(seedToKey(0x00000000), quint32(0xAA04C31C)); // parity 0
        QCOMPARE(seedToKey(0x00000009), quint32(0x2104C31C)); // parity 1
        QCOMPARE(seedToKey(0x12345678), quint32(0x8C536B33)); // parity 2
        QCOMPARE(seedToKey(0x00090914), quint32(0x1E0C3241)); // parity 3
        QCOMPARE(seedToKey(0x09090909), quint32(0x1B632D75)); // parity 4 (default branch)
        QCOMPARE(seedToKey(0xD61B2EEA), quint32(0xBA80A2C1)); // live ECU log sample
    }
    void extract_seed_reads_big_endian_bytes_4_to_7() {
        QCOMPARE(extractSeed(QByteArray::fromHex("0000000012345678")), quint32(0x12345678));
    }
    void extract_seed_returns_zero_for_short_reply() {
        QCOMPARE(extractSeed(QByteArray::fromHex("001122")), quint32(0));
    }
    void security_key_frame_layout() {
        QCOMPARE(buildSecurityKeyFrame(0x8C536B33), QByteArray::fromHex("13008C536B330000"));
    }
    void security_granted_checks_byte_3() {
        QVERIFY(securityGranted(QByteArray::fromHex("0000000100000000")));
        QVERIFY(securityGranted(QByteArray::fromHex("FF000002D61B2EEA")));
        QVERIFY(!securityGranted(QByteArray::fromHex("0000000000000000")));
    }
    void security_granted_false_for_short_reply() {
        QVERIFY(!securityGranted(QByteArray::fromHex("0011")));
    }
    void log_reset_frame_layout() {
        QCOMPARE(buildLogResetFrame(0), QByteArray::fromHex("1400000000000631"));
        QCOMPARE(buildLogResetFrame(2), QByteArray::fromHex("1400020000000631"));
    }
    void log_start_frame_uses_milliseconds_when_it_fits_in_16_bits() {
        QCOMPARE(buildLogStartFrame(0, 1, 10), QByteArray::fromHex("060001000100000A"));
    }
    void log_start_frame_switches_to_tens_of_ms_above_65535() {
        // 100000 ms > 65535, so unit flag = 1 and the field carries 100000/10 = 10000 = 0x2710.
        QCOMPARE(buildLogStartFrame(0, 3, 100000), QByteArray::fromHex("0600010003012710"));
    }
    void batching_packs_channels_into_one_frame_when_they_fit() {
        QVector<CdbgChannel> channels = { {0x804FBF, 1}, {0x804DF2, 2} };
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(channels, frames));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).size(), 2);
    }
    void batching_starts_a_new_frame_when_the_next_channel_would_overflow() {
        // byteIndex starts at 1 (byte 0 is the frame-index marker). Two
        // 4-byte channels can't share a frame (1+4=5 ok, but the second
        // 4-byte channel would need 5+4=9 > 8), so it spills into frame 1
        // along with the following 2-byte channel (5+2=7 <= 8).
        QVector<CdbgChannel> channels = { {0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2} };
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(channels, frames));
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(0).size(), 1);
        QCOMPARE(frames.at(1).size(), 2);
    }
    void batching_rejects_empty_channel_list() {
        QVector<CdbgChannel> channels;
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(!batchChannelsIntoFrames(channels, frames));
    }
    void batching_rejects_more_than_kMaxFrames_frames() {
        // 9 single-byte-incompatible channels forcing 9 frames (one 4-byte
        // channel per frame, since 1+4=5 <= 8 but a second 4-byte channel
        // would overflow) - one more than kMaxFrames (8).
        QVector<CdbgChannel> channels;
        for (int i = 0; i < 9; ++i)
            channels.append(CdbgChannel{quint32(0x800000 + i), 4});
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(!batchChannelsIntoFrames(channels, frames));
    }
    void frame_init_frames_layout_for_two_items() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1}, {0x804DF2, 2} };
        QVector<QByteArray> cmds = buildFrameInitFrames(0, 0, frameItems);
        QCOMPARE(cmds.size(), 4);
        QCOMPARE(cmds.at(0), QByteArray::fromHex("1500000000000000")); // select item 0
        QCOMPARE(cmds.at(1), QByteArray::fromHex("1600010000804FBF")); // pointer/size item 0
        QCOMPARE(cmds.at(2), QByteArray::fromHex("1500000001000000")); // select item 1
        QCOMPARE(cmds.at(3), QByteArray::fromHex("1600020000804DF2")); // pointer/size item 1
    }
    void decode_frame_reads_big_endian_values_at_the_right_offsets() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1}, {0x804DF2, 2} };
        QByteArray frame = QByteArray::fromHex("002A123400000000");
        QVector<quint32> values = decodeFrame(0, frameItems, frame);
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0), quint32(0x2A));
        QCOMPARE(values.at(1), quint32(0x1234));
    }
    void decode_frame_rejects_mismatched_frame_index() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1} };
        QByteArray frame = QByteArray::fromHex("012A000000000000"); // index byte is 1, not 0
        QVERIFY(decodeFrame(0, frameItems, frame).isEmpty());
    }
    void decode_frame_rejects_too_short_frame() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 4} };
        QByteArray frame = QByteArray::fromHex("0011"); // needs 1+4=5 bytes, only has 2
        QVERIFY(decodeFrame(0, frameItems, frame).isEmpty());
    }
};
int run_test_mitsu_colt_can_cdbg_protocol(int argc, char** argv) {
    TestMitsuColtCanCdbgProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_cdbg_protocol.moc"
