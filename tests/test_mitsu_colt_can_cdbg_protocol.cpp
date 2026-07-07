#include <QtTest>
#include "protocol/mitsu_colt_can_cdbg_protocol.h"
#include "test_mitsu_colt_can_cdbg_protocol.h"
using namespace MitsuColtCanCdbg;

class TestMitsuColtCanCdbgProtocol : public QObject { Q_OBJECT
private slots:
    void init_frame_layout() {
        const CdbgFrame expected{0x01, 0x01, 0, 0, 0, 0, 0, 0};
        QVERIFY(buildInitFrame() == expected);
    }
    void security_seed_request_frame_layout() {
        const CdbgFrame expected{0x12, 0, 0x02, 0, 0, 0, 0, 0};
        QVERIFY(buildSecuritySeedRequestFrame() == expected);
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
        const CdbgFrame reply{0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
        QCOMPARE(extractSeed(reply), quint32(0x12345678));
    }
    void extract_seed_returns_zero_for_short_reply() {
        const bytes::Bytes reply{0x00, 0x11, 0x22};
        QCOMPARE(extractSeed(reply), quint32(0));
    }
    void security_key_frame_layout() {
        const CdbgFrame expected{0x13, 0, 0x8C, 0x53, 0x6B, 0x33, 0, 0};
        QVERIFY(buildSecurityKeyFrame(0x8C536B33) == expected);
    }
    void security_granted_checks_byte_3() {
        const CdbgFrame granted{0, 0, 0, 1, 0, 0, 0, 0};
        const CdbgFrame liveGranted{0xFF, 0, 0, 2, 0xD6, 0x1B, 0x2E, 0xEA};
        const CdbgFrame denied{0, 0, 0, 0, 0, 0, 0, 0};
        QVERIFY(securityGranted(granted));
        QVERIFY(securityGranted(liveGranted));
        QVERIFY(!securityGranted(denied));
    }
    void security_granted_false_for_short_reply() {
        const bytes::Bytes reply{0x00, 0x11};
        QVERIFY(!securityGranted(reply));
    }
    void log_reset_frame_layout() {
        const CdbgFrame instance0{0x14, 0, 0, 0, 0, 0, 0x06, 0x31};
        const CdbgFrame instance2{0x14, 0, 2, 0, 0, 0, 0x06, 0x31};
        QVERIFY(buildLogResetFrame(0) == instance0);
        QVERIFY(buildLogResetFrame(2) == instance2);
    }
    void log_start_frame_uses_milliseconds_when_it_fits_in_16_bits() {
        const CdbgFrame expected{0x06, 0, 1, 0, 1, 0, 0, 0x0A};
        QVERIFY(buildLogStartFrame(0, 1, 10) == expected);
    }
    void log_start_frame_switches_to_tens_of_ms_above_65535() {
        // 100000 ms > 65535, so unit flag = 1 and the field carries 100000/10 = 10000 = 0x2710.
        const CdbgFrame expected{0x06, 0, 1, 0, 3, 1, 0x27, 0x10};
        QVERIFY(buildLogStartFrame(0, 3, 100000) == expected);
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
        const std::vector<CdbgFrame> cmds = buildFrameInitFrames(0, 0, frameItems);
        QCOMPARE(cmds.size(), std::size_t(4));
        const CdbgFrame select0{0x15, 0, 0, 0, 0, 0, 0, 0};
        const CdbgFrame pointer0{0x16, 0, 1, 0, 0, 0x80, 0x4F, 0xBF};
        const CdbgFrame select1{0x15, 0, 0, 0, 1, 0, 0, 0};
        const CdbgFrame pointer1{0x16, 0, 2, 0, 0, 0x80, 0x4D, 0xF2};
        QVERIFY(cmds.at(0) == select0); // select item 0
        QVERIFY(cmds.at(1) == pointer0); // pointer/size item 0
        QVERIFY(cmds.at(2) == select1); // select item 1
        QVERIFY(cmds.at(3) == pointer1); // pointer/size item 1
    }
    void decode_frame_reads_big_endian_values_at_the_right_offsets() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1}, {0x804DF2, 2} };
        const CdbgFrame frame{0, 0x2A, 0x12, 0x34, 0, 0, 0, 0};
        QVector<quint32> values = decodeFrame(0, frameItems, frame);
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0), quint32(0x2A));
        QCOMPARE(values.at(1), quint32(0x1234));
    }
    void decode_frame_rejects_mismatched_frame_index() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1} };
        const CdbgFrame frame{1, 0x2A, 0, 0, 0, 0, 0, 0}; // index byte is 1, not 0
        QVERIFY(decodeFrame(0, frameItems, frame).isEmpty());
    }
    void decode_frame_rejects_too_short_frame() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 4} };
        const bytes::Bytes frame{0x00, 0x11}; // needs 1+4=5 bytes, only has 2
        QVERIFY(decodeFrame(0, frameItems, frame).isEmpty());
    }
};
int run_test_mitsu_colt_can_cdbg_protocol(int argc, char** argv) {
    TestMitsuColtCanCdbgProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_cdbg_protocol.moc"
