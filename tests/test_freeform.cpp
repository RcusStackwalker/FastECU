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
};
int run_test_freeform(int argc, char** argv) {
    TestFreeform t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_freeform.moc"
