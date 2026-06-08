#include <QtTest>
#include "protocol/mut_dma_codec.h"
#include "test_codec.h"
using namespace mutdma;
class TestCodec : public QObject { Q_OBJECT
private slots:
    void sum8_wraps() {
        QByteArray b = QByteArray::fromHex("01020304");
        QCOMPARE(sum8(b, 0, b.size()), quint8(0x0A));
        QByteArray big = QByteArray(3, char(0xFF)); // 0x2FD -> 0xFD
        QCOMPARE(sum8(big, 0, big.size()), quint8(0xFD));
    }
    void command_frame_layout() {
        QByteArray payload = QByteArray::fromHex("AABB");
        QByteArray f = buildCommandFrame(0x81, payload, TRAILER_STD);
        QCOMPARE(f.size(), FRAME_LEN);            // 51
        QCOMPARE(quint8(f.at(0)), quint8(0x81));  // cmd
        QCOMPARE(quint8(f.at(1)), quint8(0xAA));  // payload
        QCOMPARE(quint8(f.at(2)), quint8(0xBB));
        QCOMPARE(quint8(f.at(3)), quint8(0x00));  // zero pad
        QCOMPARE(quint8(f.at(49)), sum8(f, 0, 49)); // checksum over bytes 0..48
        QCOMPARE(quint8(f.at(50)), quint8(TRAILER_STD));
    }
    void verify_accepts_built_frame() {
        QByteArray f = buildCommandFrame(0xA0, QByteArray::fromHex("04"), TRAILER_FREEFORM);
        QVERIFY(verifyFrame(f));
        f[49] = char(quint8(f.at(49)) ^ 0xFF);     // corrupt checksum
        QVERIFY(!verifyFrame(f));
    }
};
int run_test_codec(int argc, char** argv) {
    TestCodec t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_codec.moc"
