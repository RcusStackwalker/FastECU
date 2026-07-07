#include <QtTest>
#include "protocol/mut_dma_codec.h"
#include "test_codec.h"
using namespace mutdma;
class TestCodec : public QObject { Q_OBJECT
private slots:
    void sum8_wraps() {
        const bytes::Bytes b = {0x01, 0x02, 0x03, 0x04};
        QCOMPARE(sum8(b), bytes::Byte(0x0A));
        const bytes::Bytes big = {0xFF, 0xFF, 0xFF}; // 0x2FD -> 0xFD
        QCOMPARE(sum8(big), bytes::Byte(0xFD));
    }
    void command_frame_layout() {
        const bytes::Bytes payload = {0xAA, 0xBB};
        const MutDmaFrame f = buildCommandFrame(0x81, payload, TRAILER_STD);
        QCOMPARE(static_cast<int>(f.size()), FRAME_LEN); // 51
        QCOMPARE(f[0], bytes::Byte(0x81));  // cmd
        QCOMPARE(f[1], bytes::Byte(0xAA));  // payload
        QCOMPARE(f[2], bytes::Byte(0xBB));
        QCOMPARE(f[3], bytes::Byte(0x00));  // zero pad
        QCOMPARE(f[49], sum8(f, 0, 49)); // checksum over bytes 0..48
        QCOMPARE(f[50], TRAILER_STD);
    }
    void verify_accepts_built_frame() {
        MutDmaFrame f = buildCommandFrame(0xA0, bytes::Bytes{0x04}, TRAILER_FREEFORM);
        QVERIFY(verifyFrame(f));
        f[49] = static_cast<bytes::Byte>(f[49] ^ 0xFF); // corrupt checksum
        QVERIFY(!verifyFrame(f));
    }
    void stream_frame_parse() {
        // [LOG_ID][data...][sum8(id..lastData)][0x0D]
        const bytes::Bytes data = {0x12, 0x34, 0x00, 0x56};
        bytes::Bytes f = {0x51};
        f.insert(f.end(), data.begin(), data.end());
        f.push_back(sum8(f));
        f.push_back(TRAILER_STD);
        StreamFrame s = parseStreamFrame(f);
        QVERIFY(s.ok);
        QCOMPARE(s.logId, bytes::Byte(0x51));
        QVERIFY(s.data == data);
        f[f.size() - 2] = static_cast<bytes::Byte>(f[f.size() - 2] ^ 0xFF); // corrupt checksum
        QVERIFY(!parseStreamFrame(f).ok);
    }
};
int run_test_codec(int argc, char** argv) {
    TestCodec t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_codec.moc"
