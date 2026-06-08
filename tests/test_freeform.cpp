#include <QtTest>
#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_codec.h"
#include "test_freeform.h"
using namespace mutdma;
class TestFreeform : public QObject { Q_OBJECT
private slots:
    void size_descriptor_mapping() {
        QCOMPARE(sizeToDescriptor(1), quint8(0));
        QCOMPARE(sizeToDescriptor(2), quint8(1));
        QCOMPARE(sizeToDescriptor(4), quint8(2));
    }
    void setup_frame() {
        QByteArray f = buildSetupFrame(0xA0, 3);
        QCOMPARE(f.size(), FRAME_LEN);
        QCOMPARE(quint8(f.at(0)), quint8(0xA0));
        QCOMPARE(quint8(f.at(1)), quint8(3));            // channel count
        QCOMPARE(quint8(f.at(TRAILER_OFFSET)), quint8(TRAILER_FREEFORM));
        QVERIFY(verifyFrame(f));
    }
    void reqlen_formula() {
        QCOMPARE(reqLen(1), ((1+3)>>2) + 1*2 + 0x1c);    // 1 + 2 + 28 = 31
        QCOMPARE(reqLen(4), ((4+3)>>2) + 4*2 + 0x1c);    // 1 + 8 + 28 = 37
    }
    void id_list_frame() {
        QVector<Channel> ch = { {0x8000, 2}, {0x8004, 1} }; // N=2
        QByteArray f = buildIdListFrame(0xA1, ch);
        QCOMPARE(f.size(), reqLen(2));                 // ((2+3)>>2)+4+28 = 1+4+28 = 33
        QCOMPARE(quint8(f.at(0)), quint8(0xA1));       // rate selector
        QCOMPARE(quint8(f.at(1)), quint8(2));          // count
        // descriptors: ch0=2B->1 at bits[7:6], ch1=1B->0 at bits[5:4] => 0b01000000 = 0x40
        QCOMPARE(quint8(f.at(2)), quint8(0x40));
        // ids start at offset 2 + ceil(N/4) = 2 + 1 = 3, big-endian u16
        QCOMPARE(quint8(f.at(3)), quint8(0x80)); QCOMPARE(quint8(f.at(4)), quint8(0x00));
        QCOMPARE(quint8(f.at(5)), quint8(0x80)); QCOMPARE(quint8(f.at(6)), quint8(0x04));
        // checksum at reqLen-2 over bytes [0..reqLen-3]; trailer 0x0D at reqLen-1
        QCOMPARE(quint8(f.at(f.size()-2)), sum8(f, 0, f.size()-2));
        QCOMPARE(quint8(f.at(f.size()-1)), quint8(TRAILER_STD));
    }
    void decode_stream_values() {
        QVector<Channel> ch = { {0x8000,2}, {0x8004,1}, {0x8008,4} };
        QCOMPARE(responseDataLength(ch), 2 + 1 + 4);     // 7
        QByteArray data = QByteArray::fromHex("1234" "56" "89ABCDEF"); // BE per channel
        QVector<quint32> v = decodeStreamValues(ch, data);
        QCOMPARE(v.size(), 3);
        QCOMPARE(v.at(0), quint32(0x1234));
        QCOMPARE(v.at(1), quint32(0x56));
        QCOMPARE(v.at(2), quint32(0x89ABCDEF));
    }
};
int run_test_freeform(int argc, char** argv) {
    TestFreeform t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_freeform.moc"
